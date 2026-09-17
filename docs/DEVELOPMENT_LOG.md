# Metro 2033 Redux — VR Mod Status Log

Reference file for seeding a new session. Dense by design.
Last updated through 2026-09-12. The original summary header was written on
2026-08-23 and the dated investigation entries after it were appended in order.

> **Historical record, not active instructions.** This file preserves the
> detailed investigation history so a developer or AI can avoid repeating
> failed experiments and recover the reasoning behind the current code. Some
> entries describe temporary candidates, deployment conventions, or conclusions
> that were later superseded. Start with `AI_HANDOFF.md` and `STATUS.md`, trust
> the newest applicable entry, and verify every claim against the current source.
> References to `Builds/`, `GameReferences/`, private videos, captures, and
> dumps document evidence that existed during development; those private or
> generated artifacts are intentionally not included. The authored `Notes/`
> documents are published under `docs/history/`.

For the complete final architecture, calibration controls, proven draw
identities, failure history, and future VR-menu toggle plan for the separated
left hand and wrist watch, read
`history/31-separated-left-hand-and-watch.md` before modifying that subsystem.
The distinct prologue hands/watch and the active first attachment candidate are
recorded in `history/32-prologue-left-hand-and-watch.md`.

---

## 1. PROJECT OVERVIEW

| Field | Value |
|---|---|
| Target game | Metro 2033 Redux (4A Engine, D3D11) |
| Mod base | Fork of bo3b/3Dmigoto — a D3D11 wrapper DLL (`d3d11.dll`) |
| VR runtime | OpenVR / SteamVR |
| Headset | Samsung Galaxy XR via Virtual Desktop → SteamVR |
| Perf target | 72 Hz |
| Renderer type | **Deferred** (G-buffer: view-space position + normals, then lighting/shadows) |
| Stereo mode | Single-pass by default, hybrid fallback to double-draw for 5 shaders |
| DRM | Steam wrapper — `.bind` section; on-disk `.text` is **encrypted** |

Engine facts that constrain everything:

- Culling is **CPU-side and upstream of D3D submission**. A D3D11 wrapper can only observe what reaches D3D; anything culled before submission is invisible from our vantage point.
- The engine uses **GPU occlusion queries** (`D3D11_QUERY_OCCLUSION`) and **portal/sector visibility**, but neither drives the pop-in behavior (both eliminated — see §4).
- The engine writes **~220 projections per frame**; several passes draw real scene geometry.
- Static analysis of `metro.exe` on disk is impossible (DRM). All disassembly is done by dumping decrypted bytes from the **live process** and disassembling offline.

---

## 2. CURRENT ENVIRONMENT & SETUP

### Paths

| Purpose | Path |
|---|---|
| Mod source root | `<PRIVATE_DEVELOPMENT_ROOT>\ThirdParty\3Dmigoto\` |
| Solution | `ThirdParty\3Dmigoto\StereovisionHacks.sln` (project: `DirectX11`) |
| Build output | `ThirdParty\3Dmigoto\builds\x64\Release\d3d11.dll` |
| Game install | `<METRO_INSTALL>\` |
| Runtime log | `<METRO_INSTALL>\d3d11_log.txt` |
| Active game config | `%LOCALAPPDATA%\4A Games\Metro 2033\<profile-id>\user.cfg` |
| Learned meshes | `<METRO_INSTALL>\vr_viewmodel_meshes.txt` |
| Notes | `<PRIVATE_DEVELOPMENT_ROOT>\Notes\` (historical; not published) |
| RenderDoc | `<PRIVATE_DEVELOPMENT_ROOT>\Tools\RenderDoc\qrenderdoc.exe` |

### Key source files

| File | Contains |
|---|---|
| `DirectX11/HackerContext.cpp` | D3D11 context wrapper; per-draw hooks; `PatchMappedVRCameraData`, `PatchMappedVRObjectData`, UI constant-buffer handling, `Map`/`Unmap`, `GetData`, `SetPredication` |
| `DirectX11/VRPose.cpp` / `.h` | OpenVR integration, head/controller poses, weapon transform, aiming, shot redirect, input injection, virtual gamepad |
| `DirectX11/StereoSinglePass.cpp` / `.h` | Single-pass stereo; `ForceDoubleDrawForShader()`, `LegacyDeferredSpaceEnabled()` |
| `DirectX11/StereoTwin.cpp` / `.h` | Double-draw (twin pass) machinery |
| `DirectX11/cursor.h` | `InstallHookLate()` — Nektra inline hooking helper |
| `DirectX11/DLLMainHook.h` | `extern CNktHookLib cHookMgr` — used to hook arbitrary addresses |

### Build

```
MSBuild.exe StereovisionHacks.sln /t:DirectX11 /p:Configuration=Release /p:Platform=x64
```

From a bash-like shell, prefix `MSYS2_ARG_CONV_EXCL="*"` or MSBuild's `/t:` and `/nologo` flags get path-mangled.

### Deploy — explicit copy required after every build

The post-build `CopyToGames.bat` is gated to `username == bo3b` and silently no-ops. **The build never deploys.**

```
cp .../builds/x64/Release/d3d11.dll "<METRO_INSTALL>/d3d11.dll"
cmp -s <src> <dst> && echo deploy confirmed || echo DEPLOY FAILED
```

- The game **must be fully closed** (the DLL is locked otherwise).
- The working convention is now: **build and deploy automatically without asking** whenever Metro is closed. If it is open, finish the build and wait only for the process to close.
- Always verify source and destination with SHA-256. Testing an undeployed build has produced false conclusions more than once; file size alone is not sufficient evidence.
- Renaming `d3d11.dll` disables the mod entirely — including any in-mod diagnostics.

### Tooling notes

- RenderDoc runs headless: `qrenderdoc.exe --python <script>`. Plain `python` lacks the `renderdoc` module. Only one replay context at a time — kill stray `qrenderdoc.exe` processes before retrying. Hangs are often a blocking "Bug Reporter" dialog (check `MainWindowTitle`); kill and retry, usually succeeds within 2–4 attempts.
- RenderDoc **cannot attach to a running process** and conflicts with the mod at launch, so live-with-mod captures are not available.
- `capstone` is installed for Python; used to disassemble in-process byte dumps.
- Game config `user.cfg` holds both keybinds (including gamepad presets) and `r_*` cvars. `r_light_frames2sleep` is set to `0` (benign, no effect on artifacts). `r_base_fov` was tested and **removed** (clamps at 121.9°, no effect).

---

## 3. COMPLETED & WORKING FEATURES

### 3.1 Pop-in / flicker — CPU software occlusion bypass

**Root cause:** `metro.exe+0x826B50` — a CPU-side software occlusion routine that projects an object's bounding box into screen space, tests it against an engine-maintained CPU depth buffer, and returns "hidden". Hidden objects **never reach D3D11**. In VR that buffer represents the game's original narrow view, so objects visible through the headset were rejected.

**Fix:** patch the routine to return visible unconditionally.

```
mov eax, 1
ret
```

`InstallCpuScreenOcclusionBypass()` in `VRPose.cpp`. Signature-checks the prologue before writing, so a game update fails safe and logs rather than corrupting an unrelated function. All six engine callers treat nonzero as visible, so patching the shared routine covers every render-list category. Touches no camera, transform or input state.

### 3.2 Moving shadows / lighting — mixed-space G-buffer

**Root cause:** the geometry path patched **both** `m_WV` (world→view, feeds deferred position/normals) and `m_WVP` (world→view→projection, screen placement). Regular geometry then wrote VR-corrected view-space data into the G-buffer, while instanced geometry kept the original engine view space baked into its instance data and deferred lights/shadow matrices still expected the original space. Lighting interpreted a **mixed-space** G-buffer as if it were all original space; as the headset rotated, the disagreement changed and lights/shadows slid or vanished.

**Fix in `PatchMappedVRObjectData`:**

- **preserve** original `m_WV` (floats 24..35)
- **keep writing** corrected per-eye `m_WVP` (floats 36..51)

Gate: `StereoSinglePass::LegacyDeferredSpaceEnabled()` (returns true). `PatchMappedDeferredLightCB` early-returns under it, leaving `cb_light` untouched.

Fixed: moving shadows, lamp shadow behavior, ammunition-seller light and its illumination, several head-tracked/moving assets.

### 3.3 Single-pass stereo — safe hybrid

Single-pass issues one instanced draw targeting both array slices; valid only when the shader's sole eye-dependent output is `SV_Position`. Shaders that also emit view-space position/normals/depth produce correct left-eye and partially wrong right-eye data.

Five shaders forced to double-draw (`ForceDoubleDrawForShader()`):

| Hash | Pass |
|---|---|
| `320C272616C11623` | textured world |
| `4E9D4E63C6F1477E` | position/depth world variant |
| `6FC1713CCCC530EE` | small textured/decal geometry |
| `67B434E01A3150AF` | dominant corridor/world-surface variant |
| `367328457692E6B2` | closely related corridor variant |

Single-pass is permanent default; no F6/F7/F8 diagnostic hotkeys remain.

### 3.4 World-placed UI (main menu)

Menu labels carry an engine-generated world→clip matrix built with Metro's **flatscreen** projection, while the surrounding room was converted to HMD projection — two different projection spaces. Sharing one matrix across eyes is also wrong (labels are world-placed and need parallax).

Per eye: remove the game's projection, apply that eye's HMD projection, include the VR view correction, preserve the rest of the per-item transform, rebind a private UI buffer.

`BuildWorldUIScreenMatrix()` + `BeginUniversalUICB()` in `HackerContext.cpp`.

Gameplay HUD (ammo, watch, prompts) is deliberately different: placed on a view-locked VR plane rather than treated as world geometry.

### 3.5 UI classification by write-capture

The engine reuses **one** `cb_misc_0` for every UI draw and rewrites it between draws (confirmed: same buffer pointer carrying w-rows of 0.38, 20.17, −23.20, 9.02 within a few frames). Therefore:

- a verdict cached **per buffer** is meaningless
- an **async readback** landing 2 frames later describes a different element

**Solution:** capture what the engine *writes*. `HackerContext::Map` records the CPU pointer for the buffer bound at VS slot 3; `Unmap` snapshots the 272 bytes before flush (`sUILastCB`, `sUILastFlat`). The write immediately precedes the draw that uses it — exact, zero readback cost.

Classifier: `m_screen`'s w-output row (floats 48..51). Flat HUD = `[0,0,0,1]`; world-placed = genuinely non-zero.

### 3.6 Virtual Xbox gamepad (input)

`metro.exe` imports `XINPUT1_3.dll`. Hooks `XInputGetState` + `XInputGetCapabilities` in-process via `cHookMgr`. Lets the game apply its own **gamepad preset 3**, reaching actions with no keyboard binding (Equipment Inventory, Lighter). Buttons and **left-stick locomotion** are synthesised; the right stick remains functionally neutral so the mod's mouse-based aiming isn't fought (apart from the one-poll, sub-deadzone `RX=1` release sentinel documented in §5.19). Left-stick input uses a radial, rescaled deadzone and a 100 ms watchdog. `dwPacketNumber` changes only when the published state changes, so the pad reads idle when untouched.

Mapping: left X/Y = Use-Reload-Interact / Weapons Inventory; left grip = Equipment Inventory; left stick click = Sprint; right A/B = Jump / Melee; right grip = Secondary; right stick click = Crouch Toggle; **LT = modifier** (never forwarded) with LT+stick = D-pad (Light / Lighter / Swap Filter / Medkit), LT+A/B = Journal / Menu. Mod controls behind the modifier: LT+right stick click = recentre; LT+left stick click (hold ~2 s) = stereo mode toggle; LT+left grip (hold ~1 s) = sight-zeroing mode.

### 3.7 Other fixes

| Fix | Detail |
|---|---|
| Culling follow lag | `kMaxStep` 40 → 250 (~3.6°/frame → ~22°/frame). Worst error while turning went **40–52° → 2–4°**. Sampling every 600 frames had always caught it at rest (0.6°) and reported it healthy. |
| `m_iP` inverse mismatch | `m_P` included the view correction; `m_iP` was the inverse of the eye projection **alone**. Now inverted from the corrected matrix directly. Matters in a deferred renderer (position reconstruction from depth). |
| Levelled reference rotation | HMD reference now keeps **yaw only**, discarding startup pitch/roll. Previously the player's head tilt at launch defined "up", so physically turning introduced roll. Second, independent cause from the earlier `R_ref^T · R_cur` ordering fix. |
| Settle counter | `GetAimOffsetFromController`'s settle incremented **once per shot** (only caller), so the first ~30 rounds of a session went unredirected. Now anchors on first use. |

---

## 4. TROUBLESHOOTING & FAILED ATTEMPTS

### 4.1 Pop-in / flicker — eliminated hypotheses

| Hypothesis | Method | Result |
|---|---|---|
| Frozen/stale clip planes | Rebuild on change | No effect |
| Temporal accumulation across eyes | Disable alternation | Persisted |
| Stale global cached projection | Exclude minority projections | Caused doubling; flicker unaffected |
| Per-object depth mismatch | Recover each object's own depth mapping | No effect |
| **GPU occlusion queries** | Intercepted **70,000** `GetData` readbacks, forced all to "fully visible" | **No change, no framerate cost.** `SetPredication` called **0** times — no second GPU path |
| Gross frustum culling | Draw counts | Steady 1650–1900, spread 100–300 — **misleading, see 4.4** |
| LOD mesh swapping | Distinct index-count churn per frame | 0–4 of ~365 — **weak evidence, see 4.4** |
| Light sleep | `r_light_frames2sleep` 10 → 0, verified persisted | No change |
| Culling frustum **width** | `r_base_fov` 160 then 220 | Clamps at **121.9° h / 90° v**; identical; no change |
| Culling frustum **aim** | Rendered view forward vs engine camera forward, world space | **0.3–6.5°** — aimed correctly all along |
| Follow **lag** | Raised `kMaxStep`; error 50° → 3° | Symptom unchanged (real bug, not this cause) |
| Aim injection steering camera at gun | Gun movement with head still | Does not change what renders |
| Portal/sector visibility | 38,256 calls, all sectors forced visible | No visual change |
| Twin pass / AER | Compare modes | Identical |

### 4.2 Aiming — eliminated approaches

| Attempt | Result |
|---|---|
| Weapon-mesh detection as "gameplay started" signal | Rejected — player is often gunless; also the broad `sPendingWeaponVB` check misfired on main-menu props |
| Camera world position as menu/gameplay discriminator | Failed — the diegetic main menu has a real, moving, scripted-pan camera |
| Bias in **Euler yaw/pitch** | Oscillated across sessions (totals: +11.28/−29.08 → −13.20/−17.80 → −3.85/−26.53). **Wrong shape** — see 4.4 |
| Bias rotated about the **controller's** up axis | Wrong axis — stick moved impacts diagonally; coupling varied with elevation |
| Bias rotated about the **barrel's** own up axis | Correct axis; still no elevation fix |
| Target at engine hit **depth** along view axis | **Caused** elevation compression (`dir·camFwd` shrinks with tilt). Reverted |
| Target range-matched on barrel line | No fix |
| Target on camera ray | No fix |
| Endpoint at 60 m (expecting engine to trace) | Impact went to 60 m — **proved the engine does not trace along our ray** |
| Writing ray origin + direction (`+0x0E0`/`+0x0F0`) | Engine **KEEPS** them (40/40), but impact is set by `+0x150`, so aim did not change |
| Weapon-in-body-frame mismatch | **Dead on inspection** — `vrWeaponViewCorrection3x4` is computed but deliberately unused (a prior attempt to apply it made things worse) |
| Rest anchor baking in head pitch | Weak — `D` is a delta *from* that anchor, so it cancels |

### 4.3 Fire-hook reliability (resolved)

- Hardware-breakpoint probe required **per-thread** arming from a thread snapshot, re-broadcast every 5 s → shots on newly created threads went unredirected. Replaced with an **inline hook** at `metro.exe+0x4F3E60` (`cHookMgr`).
- First diagnosis of the "warm-up" blamed thread arming; the log actually said `redirect skipped — settling`. **The skip reason was in the log and was not read.**

### 4.4 Methodological traps — do not repeat

1. **A D3D11 wrapper cannot see CPU-side culling.** "The draws aren't there" and "the draws never existed" are indistinguishable from this vantage point. Steady draw counts were read as "nothing is being removed"; a count that is steady *but permanently reduced* looks identical without a baseline. This sent the pop-in investigation into frustum geometry for hours.
2. **Index-count churn is weak evidence.** ~1700 draws share only ~365 distinct index counts, so an object popping in need not change the set.
3. **Circular measurement.** The impact point is derived from `dir`; comparing impact against `dir` (or anything computed from it) looks clean even when both are wrong together.
4. **Oscillating calibration means the wrong *shape*, not the wrong value.** The model-vs-aim discrepancy is a fixed rotation (~15.7°, range 14.7–16.3) whose yaw/pitch decomposition swings with orientation (`dYaw −16.4…+0.91`, `dPitch −0.68…−15.2`). Two Euler constants cannot represent one fixed rotation across orientations.
5. **Verify deployment before drawing conclusions.** Two sessions tested undeployed builds. Use `cmp`, not file size.
6. **Re-run old negatives when the ground shifts.** The `+0x0E0`/`+0x0F0` write was first tested while aim was metres off; a 0.42 m origin shift was invisible, so "no change" proved nothing.

---

## 5. CURRENT STATE

### 5.1 Working

- Pop-in and flicker resolved (software occlusion bypass).
- Shadows/lighting stable under head rotation (deferred-space fix).
- Single-pass stereo default, hybrid fallback; no diagnostic hotkeys.
- Main menu labels correct in both eyes, attached to their objects.
- Gameplay HUD on a view-locked VR plane.
- Full controller remap via virtual gamepad; game's own preset 3 applied.
- Every shot redirects from the first trigger pull; aim consistent across sessions.
- Aim derived from the weapon model's barrel; one calibration covers both weapons.
- View horizon stays level while turning **and on level load**.
- Engine camera pitch is closed-loop on the game's real camera, so the culling
  frustum, interaction ray and weapon transform all follow the head.
- Scripted scenes frame correctly and hand the camera back with a glide.

> **Sight zeroing is now measured against a stable reference.** Calibrations
> used to drift because the engine camera carried a residue that changed within
> a session (+13.2°, −25.1°, then +16.2°). That is fixed — see `Notes/24` §1.
> A zeroing measured after 2026-08-15 should hold.

### 5.2 Weapon / shooting — key state

| Item | Value |
|---|---|
| Fire hook | inline at `metro.exe+0x4F3E60` |
| Authoritative redirect | projectile creation at `metro.exe+0x3D7FF0`; RDX = origin pointer, R8 = direction pointer |
| Projectile layout | constructor `metro.exe+0x3D6970` copies creation direction to projectile `+0xF0`; collision consumer is `metro.exe+0x3D9A20` |
| Active flags | `kProjectileCreationRedirect = true`; `kBallisticCollisionRedirect = false`; downstream post-hit redirect disabled while creation redirect is active |
| Engine behavior | the old `+0x150` relocation moved a visual impact but was not the authoritative damage path. Redirecting at projectile creation lets Metro perform its own range/collision processing along the VR direction. |
| Aim source | canonical rendered barrel: `dir = normalize(viewToWorld(D * m))`; reticle and projectile creation call the same function |
| Current horizontal trim | original model zero was `+0.62°`; final tested correction subtracts `0.35°`, leaving `kModelBarrelYaw = 0.00471 rad` (`+0.27°`). User report: shooting is "a lot better." |
| Grip offsets | SMG `(0.142, −0.037, 0.544)`; Revolver `(0.068, 0.017, 0.790)` |
| Weapon IDs | SMG `16251`; Revolver `6273 / 1224 / 6840` |
| Hidden mesh | arms `6576` (permanently hidden) |
| Hands | **single mesh** `12132` — cannot separate left/right per draw |
| Engine ADS | Visual/input ADS remains disabled (`kUseEngineADS = false`); native ADS is pulsed synchronously around eye-level fire calls |
| Gun-to-face gesture | `IsWeaponNearFace`, engage 0.28 m / release 0.36 m; now enables native recoil suppression without moving the viewmodel |

### 5.3 Open issues

| Issue | Status |
|---|---|
| **Vertical aim error** | Shots slightly off at ceiling/floor; level and all distances accurate; both weapons identical. No credible hypothesis remaining. Only non-circular measurement left: compare computed barrel vs **actually drawn** barrel via vertex readback of the weapon compute-shader output. |
| **Native shot deviation** | The redirected endpoint still limits how much native spread survives. Native ADS recoil suppression is now working; preserving the complete native spread pattern remains separate work. |
| Watch digits | Correctly classified as flat HUD and pulled into view (bottom-right). Desired: pinned to the wrist model — requires converting a flat element to world-placed. |
| Left hand on left controller | Blocked: both hands are one mesh (`12132`); needs bone-palette work. |
| Chapters menu | Still broken (main menu itself fixed). |
| Intro/splash video | Appears doubled; likely a separate untouched shader. |
| Door/merchant icon | Missing or drifting; its vertices are a normalised 0..1 quad, not HUD pixels. |
| "160" cluster | Unidentified on-screen number cluster; not the watch. |
| **Held-camera hitching** | Slight hitching in scripted scenes that hold the camera still (the lying-down conversation). Accepted deliberately: the fix that detects it (stall detection) also breaks the framing of those same scenes. See `Notes/24` §4 — do not re-add the stall detector as-is. |
| Scripted-state flag | Not found. Three in-process differential scans failed; the shortlist was cleared by direct per-frame watching. `Notes/24` §5 records the method, the negative result and the two better routes (ground-truth labels, or breakpointing the camera-pitch write). Do not rerun the correlation scan unchanged. |
| **Menu mouse-cursor drift + prompt flipping** | UNRESOLVED. Synthetic mouse pitch injection reaches menus the pause guard does not know about (merchant, diary, map, death screen — `sPauseMenuExpected` is inferred from START/B only). Cursor walks down the merchant screen, and each synthetic MOUSE event flips Metro's button prompts to keyboard while the virtual pad reports controller. Introduced by the closed pitch loop — see §5.16. Two anti-windup attempts have not stopped it. |
| Scripted pitch pass-through sensitivity | Gated on sustained divergence (§5.16). Marginal band still waits ~1 s, which is correct for false-positive rejection but means a slow scripted beat engages late. |

### 5.4 Related notes

| File | Contents |
|---|---|
| `Notes/11` | Pop-in root cause, full elimination table, historical investigation |
| `Notes/21` | Single-pass unsafe shaders, world-placed UI, deferred-space fix, weapon-aim calibration shape problem |
| `Notes/22` | Complete gun/shooting reference: attachment, transform, shot pipeline, spread, vertical issue |
| `Notes/23` | AER physical-left-eye head-motion jitter: full diagnostics, eliminated causes, restored baseline, and next tests |
| `Notes/24` | **Camera pitch loop**: the open-loop accumulator behind world tilt, bottom-of-view pop-in, crouch-to-interact, high gun and drifting sight zeroing; additive view build; scripted-camera handover; rejected stall detector; failed flag hunt |
| §5.16 (this file) | Scripted pitch pass-through gate, its constants, and the anti-windup story behind menu cursor drift and button-prompt flipping |
| `Notes/13` | Motion aiming; documented culling limitation of injection-based aiming |
| `Notes/20` | Status log / perf path to 72 Hz |

### 5.5 AER left-eye jitter — paused 2026-08-13

AER's reticle and VR menu are fixed and fused, but the world still
doubles/jitters in the physical left eye during head movement. Extensive
instrumentation and A/B builds eliminated texture age, pose age, submit order,
GPU copy completion, compositor-frame phase, motion smoothing, tracking-space
selection, predicted-pose timing, shared per-eye history resources, and
different poses within a two-frame eye pair. See `Notes/23-aer-left-eye-jitter.md`
before resuming; it records every tested build and the next non-circular
diagnostics. The runtime has been returned to the simplest pose-corrected AER
path. `AER Phase Sync` has no demonstrated benefit and should not ship as a
 functional setting in its current form.

### 5.6 Loading screen / intro video checkpoint — paused 2026-08-16

- Loading screen is working: the panel and Artyom's narrative summary text
  are centered, enlarged, and fused in VR. The loading vertex shader is
  `D924AAD8F41ECCE2`; the narrative reuses the generic glyph vertex shader.
- Right-stick D-pad Up/Down now require a one-second hold before producing
  lighter/medkit input.
- Intro splash video shader identified as `D2B663AAD70298CE`, with pixel
  shader `3E0DD02A2D83C903`. It is a fullscreen six-vertex quad using a
  32-byte vertex stride and a 2560x1440 viewport.
- Several splash approaches were tested and rolled back: force-double draw,
  mirroring the left eye, an external shader replacement, and a runtime
  replacement using the loading matrix. The shader replacement and runtime
  replacement produced a black screen and are disabled; the game is restored
  to the last visible splash behavior.
- Normal gameplay is not doubled; only the intro video appears as two
  identical eye images. The next investigation should use a dedicated video
  compositor/panel path rather than replacing the video's vertex shader.
- Temporary fullscreen-output and splash vertex readback diagnostics were
  removed from the shipped render path after a possible frame-time regression.
  Generic HUD glyphs are now routed through the loading transform only for the
  three frames following a real loading-screen draw, instead of globally.

### 5.7 Resolution slider / compositor scaling checkpoint — paused 2026-08-16

- The original internal-target experiment was rolled back. Resizing Metro's
  `3620x2009` stereo scene targets, and scaling their viewports/constants,
  caused gameplay stereo misalignment, zooming, and doubled images. The native
  scene targets, viewports, scissors, and constant-buffer updates are restored.
- The VR menu's Resolution setting remains saved, but it no longer changes
  Metro's internal render targets. This is intentional until the correct engine
  projection/render path is found.
- A safer OpenVR submission-stage experiment was implemented in `VRPose.cpp`.
  At `0.75x`, gameplay now renders as a normal, correctly aligned image with
  reduced sharpness. This is post-render resampling: it does **not** reduce the
  game's internal rendering workload or improve FPS, and values above `1.0x`
  are only upscale/resampling rather than true supersampling.
- The compositor resampling path uses a small runtime fullscreen shader and
  leaves Metro's native eye targets untouched. It is the current deployed
  resolution behavior.
- The intro video still begins doubled and converges after a short delay.
  Several mono-presentation variants were tested, including no pose
  reprojection and one shared submission texture; none fixed it.
- The pause menu remains doubled. The pause state is detected in the log, but
  suppressing its per-eye replay and mirroring the left frame did not change
  the headset result.
- The broad pause-only twin suppression caused a brief dark transition while
  entering a level and was removed. Current gameplay behavior is restored to
  the stable compositor-scaled path; no pause-specific rendering override is
  active.
- Builds completed successfully throughout; the only recurring build warning
  is the existing `Override.h(137)` C4250 dominance warning. No backup was used.

### 5.8 Recoil-induced vertical world motion — fixed 2026-08-16

- Symptom: firing caused the world/screen to follow HMD pitch for a brief
  period after a single shot, or continuously during automatic fire, while
  the gun and hands remained controller-locked.
- Cause: the scripted-camera handover detector compared Metro's measured
  engine-camera pitch against the expected culling correction. Native weapon
  recoil moved that camera vertically, so the detector misclassified recoil
  as a scripted camera taking control and temporarily fed the engine pitch
  back into the rendered VR view.
- Fix: each intercepted shot now arms an approximately 250 ms recoil-pitch
  protection window. During that window the detector cannot start or release
  a scripted-camera handover; automatic fire refreshes it for every shot.
  Native recoil and the normal pitch-correction loop remain enabled.
- Result: tested and confirmed working for both single shots and sustained
  automatic fire. The world remains head-stable while the gun and hands stay
  attached to the controllers.

**Next focus:** investigate the newly observed gameplay flicker separately.
Do not change the internal resolution targets or re-enable the broad pause
suppression while doing so. The current deployed DLL hash is recorded by the
deployment command; rebuild/deploy only with Metro stopped.

### 5.9 Consolidated feature/history checkpoint from the long VR implementation task

This section records work discussed and tested throughout the long task that is
not fully represented by the older overview above.

#### 6DOF, viewmodel and rendering

- Full HMD translation is active. Up/down, left/right and forward/back head
  movement work; the weapon now remains controller-locked while physically
  turning. The final physical-turn fix preserves the weapon's stereo depth and
  does not regress CPU visibility/culling (`0b436df`).
- The CPU software-occlusion bypass restored most geometry, NPCs and props when
  leaning. One gray doorway/portal case remained unresolved and was deliberately
  deferred.
- Native HUD was compacted and centered after the camera-pitch/tilt repair.
  A single global translation was rejected because it centered one element
  while pushing menus out of view; the working solution changes horizontal
  scale/centering together.
- The separated-hands experiment proved that the left-hand portion of the
  combined mesh could be controller-driven, but stable wrist rotation and the
  watch attachment were not completed. Separated hands are temporarily disabled
  without deleting the implementation or saved calibration work.
- The mouse cursor is suppressed in both main and pause menus so HMD movement
  cannot steal controller selection.

#### OpenXR experiment

- A native OpenXR path was prototyped so VDXR could be used without SteamVR.
  Runtime startup and compositor presentation were reached, but the image was
  badly malformed and the existing OpenVR renderer would have required major
  reconstruction. The conversion was abandoned as not worth the risk.
- The production path remains OpenVR/SteamVR. The OpenVR implementation was not
  intentionally replaced by the abandoned OpenXR work. Prototype sources remain
  under `ThirdParty/openxr*` for possible future reference.

#### Controls and interaction

- Per-weapon position/rotation profiles persist in
  `vr_weapon_calibrations.txt`. The in-world calibration mode remains available
  while guns are being collected; saved entries accumulate, so each weapon does
  not require an immediate rebuild.
- Two-handed aiming is functional: left grip lets the left controller steer the
  front of the gun. Several incorrect mirrored/swinging implementations were
  replaced by a controller-to-controller orientation construction.
- Fire haptics are present.
- Recoil suppression uses Metro's native ADS/dispersion behavior when the weapon
  is raised to eye level while keeping the rendered gun attached to the
  controller. Height-based activation replaced an overly strict head-distance
  requirement (`dc674bc`).
- Current modifier intent: LT remains the modifier so the equipment/D-pad layer
  remains accessible; LT+R3 = Menu, LT+L3 = Journal. Right-stick Up produces
  D-pad Up for the lighter. Left grip has no ordinary Metro action because it is
  reserved for two-handed aiming/calibration behavior.
- Gamepad mode is implemented and persistent, can open/navigate the VR menu,
  restores the conventional viewmodel/arms path, and has an option to disable
  right-stick vertical look. Left-handed weapon ownership and analog-stick swap
  are separate options.

#### VR menu and reticle

- The head-locked dark gray/red VR menu opens with L3+R3. Grips change tabs;
  motion controllers and gamepad can navigate it; ordinary gameplay input is
  suppressed while it is open; continuous sliders repeat while the stick is
  held; settings save immediately.
- Tabs currently cover Render, Picture, Motion, Offsets and UI. Render-mode,
  AER, world-scale, motion/gamepad, turning, per-weapon offsets and reticle
  controls are wired. Resolution work remains paused as described in §5.7.
- The custom reticle is one fused stereo cue, yellow, four-mark Metro-like, and
  expands using Metro's native reticle/recoil spacing. Its authored size remains
  adjustable.
- On 2026-08-17 the UI-tab controls were clarified and made authoritative:
  `HUD POSITION X/Y/Z` and `HUD SIZE` became `POSITION X`, `POSITION Y`, and
  `SIZE`; Z was removed. X/Y are now model-space sight-zero angles (±1.00° in
  0.01° steps) applied inside `GetCanonicalBarrelViewDirection`, so they move
  both the reticle and projectile direction. `SIZE` remains visual-only. This
  build compiled and deployed successfully but still needs a user confirmation
  of slider direction/range after this note was written.
- The old custom digital-watch time recreation is disabled/unresolved. The
  native digits and watch casing are separate draws; attempts to reconstruct or
  reattach them produced HMD-locked, doubled, square or missing output.

### 5.10 Shooting investigation and final architecture — 2026-08-17

#### What was observed

- At one stage a single trigger pull appeared to create two impact points. The
  accurate-looking VR visual did not damage destructible objects; a second,
  engine-directed result did. Flatscreen testing did not show two visibly
  separated impacts.
- This established that the old inline hook's `+0x150` target relocation was a
  visual/post-hit layer, not the authoritative damage/collision path. It also
  explained the apparent invisible range wall and why long-range enemies could
  not be damaged even when the VR decal looked correct.
- Several intermediate hooks made neither result follow the gun, made only the
  visual result follow, or were orientation-dependent after physical turning.
  Those builds are retired and must not be restored merely because one visual
  decal looked aligned.

#### Reverse-engineered authoritative path

- `metro.exe+0x3D7FF0` is projectile creation. Its second and third pointer
  arguments are origin and direction.
- It calls `metro.exe+0x3D6970`, which stores the supplied direction in the
  projectile object at `+0xF0`.
- `metro.exe+0x3D9A20` consumes that direction for collision/hit processing.
  Redirecting here worked but exposed a paired-pass problem: Metro performs a
  second collision/update call with a copied descriptor on a consecutive frame.
  Reapplying camera-relative deviation rotated an already redirected vector and
  produced errors of roughly 9–16°. A propagated-vector guard fixed that test,
  but projectile creation is cleaner and is now the only active redirect.
- The creation hook redirects each projectile once before Metro's own collision
  and damage handling. This restored controller-relative damage and normal
  range while avoiding the visual/damage split.

#### Native spread and ADS behavior

- Metro's already-generated shot deviation is measured relative to its native
  camera ray and reapplied around the VR barrel direction.
- Weapon fields at `+0x718` / `+0x71C` provide Metro's aimed/hip dispersion
  scale. When the eye-level ADS gesture is active, native deviation is scaled by
  Metro's aimed value rather than replaced with a custom spread model.
- The existing synchronous native ADS pulse remains responsible for recoil
  suppression. The gun/viewmodel itself is not allowed to enter Metro's visual
  ADS translation, preventing the weapon/watch pieces from being pulled away
  from the controller.

#### Arktika.1 native-VR reference work

- The native 4A VR title at
  `<ARKTIKA_REFERENCE_INSTALL>\` was inspected as a
  reference. It uses Oculus runtime code and node-to-node locomotion, but the
  runtime choice does not prevent its aiming/projectile architecture from being
  useful.
- Static module comparison and live range probes were completed. The capture
  workflow was costly because focus changes could crash the title; a dedicated
  `ArktikaProbe` tool was built. Results are recorded in `Notes/25` through
  `Notes/28`.
- The useful architectural lesson was to redirect the authoritative projectile
  direction before collision, not to copy literal offsets between different
  engine versions. No safe direct Metro address transplant was found.

#### Final zeroing result

- The reticle and bullets were confirmed to track each other during the gunfight
  in `Video Project 34.mp4`; their shared center was slightly right of the iron
  sights. Therefore this was a common zero problem, not another split path.
- A 0.10° left correction was too small to judge. The deployed correction was
  increased to 0.35° left from the original +0.62° zero, yielding
  `kModelBarrelYaw = 0.00471f`. User result: shooting was substantially better.
- Future calibration should use the VR-menu X/Y sliders added in §5.9. Do not
  separately translate the reticle in screen space or bullets and reticle will
  diverge again.

### 5.11 Crash evidence and dump capture — 2026-08-17

- Metro crashed once at approximately 19:04:08 after the improved shooting
  build. `d3d11_log.txt` and `nvapi_log.txt` both ended abruptly (the latter in
  the middle of a line), confirming abnormal termination, but Windows produced
  no Metro Application Error, WER report or minidump. There was no logged
  `DXGI_ERROR_DEVICE_REMOVED`, projectile-hook exception or other direct cause.
- The last mod entries showed normal gameplay/ADS rendering rather than a fire
  redirect failure. The 0.35° change is only a constant and has no identified
  crash mechanism. Cause remains unknown unless it repeats with a dump.
- Logging is excessive: the session's `d3d11_log.txt` reached roughly 76 MB.
  Reverse-engineering master diagnostics are disabled, but per-frame pose,
  viewport and NVAPI/load-library messages still create heavy log volume. This
  should be reduced after functional work stabilizes; it is a concern, not a
  proven crash cause.
- Full WER dumps are now enabled specifically for `metro.exe` under
  `HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\metro.exe`:
  `DumpType=2`, `DumpCount=3`, and dump folder
  `%LOCALAPPDATA%\CrashDumps\Metro2033ReduxVR`.
  This is machine-local registry state and is **not** included in a project
  backup. On the next crash, inspect the new `.dmp` before changing code.

### 5.12 Immediate handoff state

- Deployed DLL after the reticle-calibration menu change was SHA-256 verified.
- Shooting is the best observed so far; shared reticle/projectile X/Y sliders
  are deployed but awaiting direct user validation.
- If Metro crashes again, analyze the WER dump first. Do not start another broad
  hook hunt without a call stack.
- Do not restore the old `+0x150` hit-point architecture, collision-layer double
  redirect, screen-space-only reticle offset, OpenXR prototype, or internal
  render-target resizing experiments.

### 5.13 Weapon attachment profiles and offset-menu save workflow — 2026-08-17

- Weapon calibration profiles were changed from attachment/mesh-history entries
  toward one persistent profile per recognized base weapon. Profiles use a
  stored `weaponKey`; saving a recognized weapon overwrites the existing key
  instead of creating another competing entry. Legacy profiles are loaded and
  compacted where a base identity can be inferred.
- The Bastard SMG and revolver have explicit base identities in the current
  viewmodel classifier. Their profiles therefore apply to future instances,
  including versions with different attachments, across levels and saves as
  long as the same calibration file is available.
- The VR menu Offsets tab now has six live sliders plus a `SAVE OFFSETS` row.
  Slider movement is preview-only; it does not write the profile file on every
  stick step. Activating the save row commits and overwrites the profile for
  the weapon currently held.
- The right-stick menu issue was traced to the weapon matcher reselecting the
  old saved profile after each frame. This made the right-stick input appear
  blocked even though it was reaching the menu. A live-preview lock now keeps
  unsaved edits for the same weapon, releases them when a different weapon is
  detected, and clears them after a successful save.
- Menu input also supports controller profiles exposing the thumbstick on a
  later non-trigger OpenVR axis, and tab changes reset the right-stick latch.
- Relevant implementation files are `ThirdParty/3Dmigoto/DirectX11/VRPose.cpp`,
  `VRPose.h`, `VRMenu.cpp`, and `HackerContext.cpp`.
- The latest Release x64 build compiled with 0 errors and was deployed to the
  Metro installation. Source and destination `d3d11.dll` SHA-256:
  `E15D47789F7610F3ACA4D10E0C5D6F2EC2D4B51EE1F72725AA7393577B521553`.
- Confirmed by user: right-stick offset editing works after the live-preview
  overwrite fix. Next useful validation is saving a modified weapon, picking
  up another instance with attachments, restarting a level, and starting a new
  game to verify the profile remains consistent.

### 5.14 Magnifying-scope activation — 2026-08-17

#### Confirmed behavior

- Raising a positively identified 2× optic to eye level now activates Metro's
  native visual ADS and applies magnification to the OpenVR eye projections.
  User confirmation: both discovered 2× scope models work well.
- Guns without a magnifying scope remain controller-pinned and do not zoom,
  including a gun that supports a scope but currently has none fitted.
- Detection is tied to the rendered **scope attachment**, not to the host gun,
  total mesh count, saved weapon-calibration profile, or the mere presence of
  an arbitrary sight. Reflex sights are intentionally excluded.
- General eye-level aiming still publishes the gameplay ADS state and uses the
  existing synchronous native ADS pulse for recoil/spread suppression. Visual
  scope ADS is a separate, magnifying-optic-only path.

#### Projection handling

- Metro performs optical zoom by narrowing its native projection. The VR patch
  normally replaces that projection with the OpenVR eye frustum, which is why
  native ADS initially moved the weapon but produced no magnification.
- `HackerContext.cpp` now scales both OpenVR eye projections around their own
  optical axes. Only P00/P11 are scaled; asymmetric frustum-center terms remain
  unchanged. The confirmed 2× path currently requests `1.744x`, matching the
  measured native world-camera result.
- Metro alternates distinct world and viewmodel ADS projections. Driving VR
  zoom directly from those alternating values caused left/right-eye mismatch,
  disappearing geometry, and head-dependent moving world pieces. The deployed
  path instead uses one requested scale for every draw and both eyes through
  `VRPose::GetRequestedScopeMagnification()`.
- Some native words/UI look imperfect under 2× magnification. This is accepted
  as a lesser unresolved issue for now.

#### Rejected automatic probe

- An early classifier briefly pressed Metro's native aim control for every
  unknown weapon and attempted to classify the optic from measured projection
  magnification. It correctly separated an ordinary world-camera result near
  `1.126x` from the first scope near `1.744x`, but the probe itself was unsafe.
- On ordinary weapons it left AER eyes in different native ADS animation states
  and disturbed culling. On the shotgun, right mouse is a secondary-fire path;
  the log recorded 18 projectile-creation calls when the gun was raised.
- This probing architecture has been removed. Unknown weapons never receive a
  synthetic ADS press, even once. Do not restore it.

#### Attachment signatures discovered

- A passive same-gun before/after census isolated the first 2× model. The base
  gun had 12 meshes; adding the scope produced 18. The six added attachment
  meshes are required as a complete signature:
  `36, 48, 384, 936, 960, 5808`.
- A visually different 2× model on another weapon family used a second complete
  attachment signature:
  `24, 48, 120, 420, 480, 8928`.
- Detection accepts either complete six-mesh signature. This is per optic-model
  variant, not per gun. Requiring the complete set avoids false positives from
  individual IndexCount collisions or unrelated attachments increasing the
  mesh count.

#### Passive census and future work

- A bounded passive census remains active for the first 32 gun-to-eye raises in
  a session. It logs the sorted held-weapon mesh set, selected calibration
  profile, base weapon key, and match counts for both known 2× variants.
- The census sends no game input and does not alter rendering or gameplay. If a
  later 2× variant fails to zoom, raising it once should leave enough evidence
  in `d3d11_log.txt` to add another optic variant.
- The remaining 4× scope has not been acquired. When found, raise it once; use
  its passive mesh-set capture to isolate its attachment signature, then add a
  separate 4× match and magnification value. Do not classify it by gun identity
  or mesh count.
- Current verified deployed DLL SHA-256 after adding both 2× variants:
  `376311AF49FF60804C5EA7119672D12E34DFC5C13ECA98672EF4993B61D9E99A`.
- Scope work remains uncommitted at the time of this note and should be
  checkpointed after broader scope/reflex validation.

### 5.15 World assets falsely attaching to the right controller — unresolved; speculative fixes rolled back

- Video `Assets Attached to Hand.mp4` showed two proximity-triggered failures:
  a recurring flame near the gun seller and a large station/building mesh both
  inherited the right-controller weapon transform when the player crossed a
  particular spot.
- Root cause was the weapon-instance substitution gate. `IsViewmodelPass()`
  plus the instance-matrix layout admitted the draws, and the GPU transform's
  Manhattan-distance radius had been widened to `6.0`. That let unrelated
  geometry enter the weapon neighbourhood when its instance origin approached
  the camera—the exact position-dependent behavior visible in the video.
- The intended `kViewmodelShaders` allowlist existed but was not consulted by
  `SubstituteWeaponInstanceBuffer`. It is now required in addition to pass and
  layout membership. The GPU proximity check remains necessary because Metro
  also uses viewmodel shaders for some world-held weapon models.
- The proximity radius is now `1.5`. Current live measurements put the furthest
  legitimate held-weapon piece at Manhattan distance `1.055`; prior unrelated
  geometry began at `2.0+`. This leaves approximately 0.445 units of measured
  headroom while restoring the guard's ability to reject scenery.
- Release x64 built with 0 warnings and 0 errors and was deployed with matching
  source/destination SHA-256:
  `523AC15BC0D6D7E6BCE4CF5197EE82ED007D7A75AABA3122994BF6E7166762B5`.
- Required validation: revisit both positions from the video, move the right
  controller through the same range, then confirm the held gun, hands, watch,
  both known 2× optics and ordinary attachments still remain controller-bound.

**Validation result and second fix (2026-08-18):** both artifacts survived the
shader/radius change; the flame attached even when the player was not close.
This disproved visible distance as a reliable proxy for instance translation.
The new session log and the existing one-shot geometry measurements isolated
two contaminated candidate classes: `IndexCount=60` is a 22-vertex effect-sized
mesh, while `IndexCount=21252` is a 4,012-vertex mesh approximately two metres
wide. These fit the recurring flame and large structural artifact respectively
and are not any confirmed hand, watch, base-weapon body or 2× optic signature.
Both are now excluded from `SubstituteWeaponInstanceBuffer`; the already-known
particle/effect block is excluded categorically as well. The older learned and
calibration files cannot be trusted as contrary evidence because they were
populated after the same permissive gate. Release x64 again built with 0
warnings and 0 errors and was deployed with matching source/destination hash:
`C5043553F44A08942A73F4554CF429F9092B772068011B7A4FF048AFB928857D`.
This second fix still requires headset validation at both video locations and a
quick check that no small held-weapon component disappeared.

**Final validation and rollback (2026-08-18):** both the flame and building
still attached after the second build, and the reduced proximity radius broke
part of the wristwatch. This rejects the shader/radius and candidate-index
classifications above as fixes for the observed artifacts. All changes from
this investigation were rolled back: the weapon-cluster radius is restored to
`6.0`, and the added shader-allowlist, particle-block, `IndexCount=60`, and
`IndexCount=21252` exclusions have been removed. The code is therefore back at
the pre-investigation rendering baseline; the issue remains unresolved and
needs a whole-path A/B diagnostic before any further mesh filtering. Release
x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`32800BD3CD71D0BC5CBA453B9FBC33BA637D62DB70E0C81B9C7C99554F2F1EB5`.

**Whole-path A/B diagnostic (2026-08-18, active test build):** rather than
guessing another shader, mesh, or distance discriminator,
`kWeaponPrivateVBEnabled` is temporarily `false`. This bypasses every call to
`SubstituteWeaponInstanceBuffer` while leaving motion-controller input active.
The held gun is expected to remain on Metro's native viewmodel path and not
follow the hand. Revisit the flame and building locations and move the right
controller: if either world asset still follows it, the fault is outside this
entire substitution path; if both stop, the fault is inside it and can be
instrumented at the pass/buffer boundary. Restore the flag to `true` after the
test.

The diagnostic release built with 0 warnings and 0 errors and was deployed
with matching source/destination SHA-256:
`46F29234DFB5A3E1BECE18143636E06A5F6D8FF8F5F93BB9EC681F09F639F6E3`.

**A/B result and boundary instrumentation (2026-08-18):** neither artifact
occurred while the complete private-instance path was disabled, proving the
fault is inside `SubstituteWeaponInstanceBuffer`. The path is restored. Its
readback diagnostic previously sampled only the first four submitted draws on
every frame, repeatedly observing the same early weapon parts and potentially
never observing a later contaminated draw. The active diagnostic now samples
each unique signature (instance buffer/offset, geometry range, and vertex
shader) once, advancing across frames with at most eight synchronous reads per
frame and a 512-signature session cap. Each line is tagged `VRPose boundary`
and records whether the GPU radius actually transforms it, its exact geometry
and instance-buffer identity, VS/PS hashes, viewport depth range, translation,
and pivot distance. Reproduce both artifacts once; the resulting log should
finally contain the actual transformed boundary draws instead of inferred mesh
classes.

The boundary-instrumented release built with 0 warnings and 0 errors and was
deployed with matching source/destination SHA-256:
`25988EE1268FD3DF1221AB5DA0AC316FD7953571D69CA1D96A0457512B7E833D`.

**Capture result and state-restoration fix (2026-08-18):** the capture reached
its 512-entry cap, but every transformed geometry signature resolved to a
legitimate viewmodel component. In particular, `IndexCount=60` measured only
about 1.9 cm and belongs near the wrist/watch, while `IndexCount=21252` is the
roughly two-metre combined arms/viewmodel geometry. This explains why excluding
those counts damaged the watch and did not affect either world artifact. The
evidence instead points to state escaping after a legitimate weapon dispatch.
Both the first-eye substitution and second-eye redispatch overwrote CS shader,
UAV 0, and constant-buffer 0, then cleared only the shader/UAV and left the
controller-filled `WeaponParams` buffer bound at CS b0. They now save and
restore the exact pre-existing compute shader (including class instances), UAV
0, and CB 0 around every dispatch. The completed boundary sampler has been
removed, including its synchronous readbacks. Required validation: revisit
both artifact locations, confirm the gun and watch remain attached correctly,
and note any material performance change from the state preservation.

The state-preserving release built with 0 warnings and 0 errors and was
deployed with matching source/destination SHA-256:
`B5DF3202CFBEC24FF6AB83BA4FC3E1C2D493B6538AF56036B5B0C34724765A7F`.

**State-fix result and redispatch lifetime root cause (2026-08-18):** both
artifacts survived full CS shader/UAV/CB preservation, so that experiment was
removed to avoid its per-draw state-query cost. Inspection then found a direct
cross-draw lifetime bug. Every weapon substitution set `sWeaponRedispatch`, but
when that draw had no twin target `BeginTwinPass` returned without consuming
the flag. A later unrelated draw with a twin target could therefore execute the
stale weapon redispatch, bind the controller-transformed private instance
buffer, and render its second eye with it. This naturally affects only assets
at particular render-order boundaries and explains why broad object filters
never helped. `RestoreWeaponInstanceBuffer` now always cancels any unconsumed
redispatch and clears its source. `BeginTwinPass` also keys and copies from the
captured `sWeaponRedispatchSource`, never the mutable current
`sPendingWeaponVB`. Required validation remains the flame and building
locations plus gun/watch integrity.

The redispatch-lifetime release built with 0 warnings and 0 errors and was
deployed with matching source/destination SHA-256:
`F291DB256528F914B384EAECF6669E0BAE25B3BA5196132667C055D83EB5664E`.

**Redispatch result and GPU/CPU path split (2026-08-18):** both artifacts also
survived the redispatch-lifetime fix, disproving it. The two artifacts are in
different levels: only the flame moves in the gun-seller room; the large
building/structural object moves elsewhere. The existing vanilla RenderDoc
capture of the gun-seller room identifies nearby fence geometry, not the
separate-level building artifact, so it must not be treated as that object's
draw signature. RenderDoc cannot capture the faulty VR frame because its D3D11
hook conflicts with 3Dmigoto, but vanilla captures can still identify each
engine draw, its buffers, layout, shaders, and surrounding render order.

The active diagnostic keeps `kWeaponPrivateVBEnabled=true` so the gun still
attaches, but sets `kWeaponGpuTransform=false`, selecting the synchronous
CPU/private-buffer implementation instead of the compute/UAV implementation.
Revisit both locations. If the artifacts disappear while the gun still follows
the controller, the defect is isolated to the GPU dispatch path. If they remain,
the common source-copy/private-substitution path is responsible. This build may
run slower because it deliberately performs synchronous readbacks per candidate
draw; it is diagnostic, not a release configuration.

The CPU-path diagnostic release built with 0 warnings and 0 errors and was
deployed with matching source/destination SHA-256:
`AAC2B77EE12FF4C6165A383901CA3B2F8791361DA3627D66652B7F4DE88CCBBB`.

**CPU/GPU split result and eye-independent GPU fix (2026-08-18):** with the
CPU/private-buffer path active, both the flame and the separate-level building
artifact disappeared. The held weapons regressed: saved placement was lost,
some guns separated from the hands, and multipart guns came apart. This proves
the shared source-copy/private-buffer mechanism is safe and isolates the world
corruption to the modern compute implementation, while also proving the legacy
CPU transform is not a viable release path.

Inspection found that the first-eye compute result was unnecessarily discarded
and recomputed inside every second-eye twin pass. The weapon transform no longer
contains an eye-specific offset; each eye's projection already supplies its own
view correction, and the old second-eye translate adjustment is disabled. Both
eyes therefore require the same completed private instance buffer. The modern
GPU path is restored for current gun placement and rigid multipart alignment,
but `sWeaponRedispatch` is no longer armed: eye 1 reuses eye 0's transformed
private buffer. This removes the only weapon compute/UAV operation interleaved
inside `BeginTwinPass`, where it could contaminate later particle/structural
draws. Required validation: gun/hand placement and multipart alignment, stereo
weapon appearance in both eyes, then both artifact locations.

The eye-independent GPU release built with 0 warnings and 0 errors and was
deployed with matching source/destination SHA-256:
`C751982A4D89799F8351FBF48FD86E7FE3081259CBE71D4DAEDF73C19EB3CA1A`.

**First-eye result and modern CPU-evaluated transform (2026-08-18):** guns
returned to their correct saved positions and remained assembled when the
second-eye redispatch was removed, but both world artifacts remained. This
rules out the twin redispatch and isolates the trigger to the first-eye UAV
compute operation itself.

The active build retains the modern GPU-path parameter construction—the exact
controller-relative placement, shared grip pivot, uniform scale, left-hand and
watch handling, and view-correction cancellation—but evaluates the final 64-byte
instance transform with an exact CPU equivalent of `kWeaponTransformHLSL`.
Only the submitted instance range is copied to a staging buffer; the game-owned
dynamic ring buffer remains read-only. The result is uploaded to the same
private vertex buffer, which is never bound as a UAV. This should combine the
correct modern gun placement with the artifact-free behavior of the CPU A/B.
It deliberately reintroduces synchronous readback cost and therefore requires
both correctness and performance validation before optimization.

The modern CPU-evaluated release built with 0 warnings and 0 errors and was
deployed with matching source/destination SHA-256:
`C96CF013237289CCF5244B93D99F7CCAAF3A58520B2A263B8232530613BCC223`.

**Modern CPU result and offset-zero admission fix (2026-08-18):** both world
artifacts survived even with no compute dispatch or UAV binding. Therefore the
UAV mechanism is exonerated; the difference from the artifact-free legacy CPU
path is candidate admission. Inspection found a specific lost guard: the legacy
path rejects `sPendingWeaponOffset == 0`, documented by earlier measurements as
Metro's shared identity placeholder for draws whose geometry is transformed by
another route. The modern path returned before ever reaching that check. It
could therefore copy the identity placeholder, place it at the controller, and
bind it to an unrelated draw—exactly the observed whole-object attachment.

The zero-offset rejection now occurs at the entrance to
`SubstituteWeaponInstanceBuffer`, before either implementation. Genuine held
weapon transforms occupy nonzero dynamic-ring offsets, so current weapon
placement and multipart alignment should remain intact. The CPU-evaluated
modern transform remains active for this controlled validation; after both
artifact locations pass, the no-stall compute dispatch can be restored behind
the same common guard.

The offset-zero diagnostic release built with 0 warnings and 0 errors and was
deployed with matching source/destination SHA-256:
`2FBB68840F72D7AD0BA7CBA7546F41D0D7C24DF7B95297AF4D104D7F5C83867E`.

**Offset-zero result and corrected admitted-draw census (2026-08-18):** both
artifacts survived the common offset-zero rejection, disproving that admission
theory. Further filtering is paused until the actual draws are identified. The
CPU-evaluated modern path now logs each distinct transformed object as
`VRPose admitted`, including index count, exact index/vertex geometry range,
VS/PS hashes, native translation, and current ring offset. Crucially, the
deduplication key excludes the dynamic ring offset that caused the earlier
boundary sampler to saturate with the same viewmodel draws; it instead uses
geometry, shaders, and translation quantised to 5 cm. The cap is 2,048 stable
signatures. Reproduce the flame and building once in a fresh run, exit, and
compare the resulting admitted signatures across the two level locations.

The corrected-census release built with 0 warnings and 0 errors and was
deployed with matching source/destination SHA-256:
`D187D94EC046079665DF5F82C2488B21345999A7B34875D2569458475E501BC9`.

**Corrected census result and downstream particle-fold root cause
(2026-08-18):** the completed log contained 223 distinct admitted signatures,
all resolving to legitimate held viewmodel components (`6108`, `60`, `750`,
`2628`, and `12132`) with their expected geometry/shader families. Neither
artifact enters `SubstituteWeaponInstanceBuffer`. The apparent whole-path A/B
coupling instead comes from publication of `sWeaponParams`: disabling weapon
substitution prevents those parameters from being produced, and a separate
consumer in `PatchMappedVRObjectData` reuses them for effects.

That consumer folded the weapon affine into every object satisfying only a
sticky particle-block flag and a 3-metre proximity test. Ambient flames share
the particle layout and naturally pass. More importantly, object constant
buffers are mapped during setup for the next draw while `sInParticleBlock`
still describes the preceding draw, so nearby structural geometry can inherit
the particle classification—the building symptom. The fold now additionally
requires `VRPose::LastShotFrame()` to be within 10 frames, matching the temporal
guard already used by `BeginMuzzleFlashCB`. Ambient effects and structures can
no longer receive the weapon transform; genuine muzzle particles retain it
immediately after firing. The admitted-draw census is removed and the no-stall
compute transform is restored.

The shot-gated particle-fold release built with 0 warnings and 0 errors and
was deployed with matching source/destination SHA-256:
`F39FB91D6EF9580148286F2F30AFA6528FA06E3CDAFA06B48A252A88C62FA596`.

**Particle-fix validation and hand regression correction (2026-08-18):** the
shot-gated object fold fixed both the ambient flame and building attachments,
confirming the downstream particle/object path as root cause. Gun placement
remained correct, but the hands separated from the guns. The earlier common
offset-zero diagnostic guard was still active even though its artifact theory
had been disproven. Metro can transiently submit combined-hand geometry through
the identity entry; rejecting the substitution leaves those hands on the native
viewmodel transform while the gun follows the controller. That guard is now
removed. The successful shot-time particle guard remains unchanged.

The hand-regression correction built with 0 warnings and 0 errors and was
deployed with matching source/destination SHA-256:
`0DB64618D49066C5DE788307FFFC7E1FBBDD634B96D5C39AE5B2A1650DAF89A7`.

**Final headset validation (2026-08-18): fixed.** The ambient flame no longer
attaches to the right controller, the separate-level building remains fixed,
and tested guns, hands, multipart weapon geometry, and saved gun positions all
behave correctly. The validated solution is the 10-frame confirmed-shot guard
on the weapon-affine fold in `PatchMappedVRObjectData`, together with removal
of the disproven offset-zero candidate guard. The normal no-stall compute
weapon transform remains enabled.

The important diagnosis is that neither world artifact was admitted by the
private weapon instance-buffer substitution. The corrected 223-signature
census contained only legitimate held viewmodel components. The artifacts were
downstream users of the published `sWeaponParams`: the generic object-matrix
particle correction used a sticky previous-draw particle flag plus a 3-metre
radius, allowing an ambient flame and nearby structural geometry to inherit the
weapon affine without any shot occurring. Do not reintroduce radius/index-count
exclusions, shader allowlists, offset-zero rejection, legacy CPU placement, CS
state preservation, or redispatch-lifetime changes as fixes for this issue;
each was tested and disproven or caused a regression. If a future ambient
effect attaches, inspect other consumers of `sWeaponParams` and require an
event-specific temporal/type discriminator rather than filtering weapon meshes.

**Disabled watch-path leakage into native HUD text (2026-08-18):** a new video
showed the white native text `321 654` and a small orange block following the
right hand. This is separate from the flame/building geometry issue and also
separate from Metro's original HMD-locked watch time described below. These
hand-bound artifacts came from the failed attempt to reconstruct/reattach the
watch output. Although the custom renderer was configured off
(`kUseCustomWatchDigits = false`), `BeforeDraw` still ran its icon-to-text
sequence detector and passed matches to `BeginUniversalUICB` as wrist-watch
digits. After the watch/icon shader armed it, any generic six-index text glyph
in the remaining draw window qualified. Consequently unrelated HUD text was
fed through `BuildWristWatchScreenMatrix` and inherited the current right-watch
casing/controller transform.

The watch block tracker and `isWatchDigitDraw` classification are now both
gated by `kUseCustomWatchDigits`. With the unresolved custom recreation off,
generic text stays on the ordinary view-locked HUD path and cannot enter the
wrist transform. This change does not touch weapon instance substitution, the
validated shot-gated particle fold, the physical watch casing, or the custom
reticle. Release x64 built with 0 warnings and 0 errors and was deployed with
matching source/destination SHA-256:
`012A4CE89C6E392BE22D3D49BCF37A83C6369BEBBD724983414AFEB2CA8B7979`.

Required validation: revisit the scene from `Video Project 38.mp4`, move the
right controller through the same range, and confirm that both the orange block
and `321 654` remain on the ordinary HUD plane rather than following the hand.
Also confirm the gun, hands, physical wristwatch, reticle, pause menu, and the
previously fixed flame/building still behave normally.

**Native HMD-locked watch time suppression (2026-08-18):** headset validation
confirmed the preceding build removed the hand-bound recreated output. A
different pre-existing orange time readout remained above the gun in some
levels. This is Metro's original watch-time UI: the physical watch casing and
the time are separate draws, and the native time uses the generic 2D HUD text
path, so it stays fixed to the HMD rather than the wrist.

`kHideNativeWatchDigits` is now enabled. The known watch-specific icon shader
arms the detector, but `IsWatchDigitDraw` now accepts exactly the next four
generic text glyphs and immediately disarms. Those four native timer draws are
skipped. Capping the sequence is essential: it removes the actual native time
without reopening the broad-window bug that swept `321 654` and other HUD text
into watch handling. The custom reconstruction remains disabled, and the
physical watch mesh is untouched.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`031885A1A1BE3BD41D7DB87196AD17F1D53F48C4D46C20428B800164130E0C1D`.

Required validation: load a level where the native orange time normally
appears and confirm it is absent. Also verify ordinary HUD text, menus, gun,
hands, physical watch casing, reticle, and the prior attachment fixes.

**First native-timer suppression result and corrected shader identity
(2026-08-18):** the orange HMD-locked time remained visible. The fresh runtime
log showed why: the supposed icon anchor stopped firing at frame 1017 while the
timer remained visible through frame 2117, so it is not a reliable per-frame
timer anchor in this level. More importantly, the existing RenderDoc artifacts
(`GameReferences/watch_digit_hash.txt` and `live_watch_ui_events.txt`) already
record the native orange timer's actual dedicated shader pair:
VS `6FC1713CCCC530EE`, PS `BD4487FCFD52C72D`. The attempted suppression was
incorrectly looking for the generic ammo/menu text pair
`7B3EB7275D556B14` / `4A48D5D4C49DCDF2`, so it could never match this timer.

Native-timer suppression now directly skips six-index draws using the captured
dedicated watch-time VS/PS pair. It no longer depends on the icon anchor or the
generic-text window. A rate-limited `VRPose watch UI: suppressed native timer
glyph` log confirms the path when exercised. The broad generic-text tracker is
again gated solely by the disabled custom reconstruction. This is both narrower
and based on the timer draw's direct identity.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`D4E9074F7AF981B334F880DB38C2D3E9A60ECC25C0D7AD0C627834BBEB7809F8`.

**Dedicated-shader result and measured-geometry suppression
(2026-08-18):** the clock still displayed the correct live wall time (`18:55`)
even though the runtime log confirmed more than 1,800 draws from the dedicated
shader pair were skipped. That pair therefore belongs to other watch display/
illumination surfaces, not the visible clock text, and its suppression has been
removed.

The saved RenderDoc capture identifies the actual clock glyph as event 759:
Metro's generic text VS/PS, 24-byte vertex stride, one six-index quad with
measured canvas bounds x `492.57..531.14`, y `358.29..414.00`. Its full
`cb_misc_0` matrix was extracted to
`GameReferences/watch_glyph_matrix.txt`; it is world-shaped and varies with
pose/level, so neither the earlier icon anchor nor a fixed matrix signature is
safe. The vertex quad is the stable discriminator already measured from the
actual clock draw.

`IsNativeWatchGlyphGeometry` now checks generic six-index text draws against
those measured bounds. Each distinct vertex-buffer location is read only once
and its verdict cached; after discovery, clock suppression has no recurring
readback cost. The tolerance is approximately ±5.5 pixels per bound and does
not match ordinary ammo/menu glyph positions. Positive identification and
suppression log as `identified native clock quad` and `suppressed measured
clock glyph` respectively.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`542F4A3A1ADDD5F6FCF6A9C0322FE50BADB2C7AA70772F1ECB0F45B7EDDC07C6`.

**Measured-glyph result and detached display-mesh suppression
(2026-08-18):** headset validation showed the orange real-time clock was still
visible even while the preceding build skipped roughly 27,000 matching generic
text quads. Those five quads per frame were therefore another shared HUD text
submission, not the glowing seven-segment object in the screenshot. The broad
geometry readback/suppression has been removed so it cannot affect ordinary
text or add a synchronous buffer map to the UI path.

The saved vanilla RenderDoc frame and the existing viewmodel analysis identify
the glowing watch display as the separate 1296-index mesh immediately following
the 6108-index watch casing. Its exact shader pair is VS
`65C62A5148831C58`, PS `3B3535E6C07D32BA`. This mesh uses the special
viewmodel instance-substitution path, which draws and returns before
`BeforeDraw`; that explains why every prior normal draw-skip experiment could
log matches without touching the visible clock.

Suppression now occurs inside `DrawIndexedInstanced`, before the viewmodel
early-return, and requires all four discriminators: the 1296 index count, one
instance, and both captured shader hashes. The 6108-index casing, hands, gun,
ordinary HUD text, and the disabled custom-watch experiment are untouched. A
rate-limited `VRPose watch: suppressed detached 1296-index display mesh` line
confirms the exact path at runtime.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`31956499A52C9E8E323A7DC26C618C4E8C9A1049F78D8EFB83D1C5A324AEA3C2`.

**RenderDoc pixel-history resolution of the native clock (2026-08-18):**
headset validation showed no change, and the runtime log contained no
`suppressed detached 1296-index display mesh` entries. That candidate was not
submitted in the affected scene and has been removed.

The mod-enabled capture `watch_live_frame5912.rdc` contains a clear physical
watch and its orange digits. Pixel history was traced backward from several
bright orange final-image pixels through the swapchain composite, lighting
combine, and G-buffer. Every stroke resolves to event 118: the 6108-index watch
draw (VS `79BA9F1ECFAF0CA1`, PS `3B3535E6C07D32BA`). Mesh connectivity over
all 2,036 triangles then identifies the four digits as four disconnected,
identically sized 56-vertex/94-triangle components:

- primitives `1550..1643`
- primitives `1644..1737`
- primitives `1738..1831`
- primitives `1942..2035`

The viewmodel early-return now splits only that exact shader/index/instance
draw. It keeps primitives `0..1549` and `1832..1941`, while omitting the four
digit components. This preserves the watch casing and every other part of the
draw; hands, gun, other 6108-index geometry using a different shader pair, and
ordinary UI text are untouched. The runtime confirmation is `VRPose watch:
omitted four native digit mesh components`.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`0B2D5CE27253CC79E26BB46E850661F3EC237613A722654D8DB0BE34FFC84071`.

**Whole-watch discriminator build (2026-08-18):** the four-component omission
fired continuously in the affected-level headset run (`#2041` by frame 2984),
but the HMD clock remained. A temporary decisive A/B now suppresses the entire
exact 6108-index watch draw before instance substitution. This intentionally
removes the physical watch for one test. If the HMD time survives, it proves a
separate later submission; if it disappears, the RenderDoc primitive IDs need
to be remapped to API index offsets before restoring the casing split.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`97A544E98FF062A9BA6DAE111F1CFC35EA2E30AB30602525DCF76E7DFC9E78EE`.

**Separate-display discriminator build (2026-08-18):** headset validation of
the whole-watch diagnostic was decisive: part of the physical wrist watch
disappeared, while the floating real-world time remained unchanged. The
6108-index diagnostic suppression has therefore been disabled and the
physical watch restored.

The remaining clock points back to the separate 1296-index viewmodel display
layer. The earlier attempt required a shader pair captured in another scene
and never fired in the affected level, showing that its material shader is not
stable across levels. The new suppression is instead inside the already
tightly bounded single-instance viewmodel path and requires its pending private
viewmodel buffer, viewmodel render pass, instance-matrix input layout, and the
1296 index count. This removes the unstable shader-hash requirement while
remaining isolated from world geometry, ordinary UI text, the watch casing,
hands, and gun components. Runtime confirmation is `VRPose watch: suppressed
1296-index viewmodel display draw`.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`08C3E9EF4D9F907AF12ED8A1F4BD6A448D7E6C1FF4EA8B6D3D28AE226348F325`.

**Affected-scene frame-analysis resolution (2026-08-18):** the 1296-index
viewmodel filter never fired in the player's runtime log and has been disabled.
A 3Dmigoto frame-analysis capture was then taken with the VR DLL active in the
exact affected scene (`FrameAnalysis-2026-08-18-195517`). Render-target
differencing gives an unambiguous sequence: draw 1244 is the known watch icon
anchor (VS `A9037683D2AF5BC0`, PS `B468E0743796D214`), and draws 1245..1249
add the five visible orange characters one by one using the generic-text pair
VS `7B3EB7275D556B14`, PS `4A48D5D4C49DCDF2`.

This exposes the key error in the first icon-anchored attempt: it assumed four
glyphs, but the native string is five draws (`HH:MM`), including the colon.
Native suppression now arms only on the exact icon shader pair, accepts the
five immediately following six-index generic-text draws, and disarms. Its
search window was reduced from 64 draws to eight. The custom wrist
reconstruction remains disabled, so these draws are skipped before any UI
constant-buffer substitution and unrelated text cannot be attached to the
controller. Runtime confirmation is `VRPose watch UI: suppressed captured
HH:MM glyph`.

The temporary frame-analysis settings were restored after capture. Release x64
built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`1F8EF36C60A4B6CCBAEA821F1A79D5751CDF0004FD54C329D21467266979DA31`.

**Continuous laser regression and targeted exemption (2026-08-18):** after
the clock fix was validated, `Video Project 39.mp4` showed the laser-sight beam
following HMD yaw instead of the gun. This is a consequence of the validated
flame/building fix: the generic near-eye particle/object fold was restricted to
ten frames after a shot, which is correct for muzzle effects but excludes a
legitimate continuous weapon laser.

The affected-scene frame-analysis capture also contains the equipped laser.
Render-target differencing identifies draw 1316 as the complete red beam: a
24-index `DrawIndexed` using VS `6C14CA9DCD06FE85` and PS
`339689302CA37E17`. Its input layout is bound before its object constant buffer
is mapped, while its shaders are bound afterward. The renderer now learns the
exact layout only when that shader/index signature completes, then permits that
layout to receive the weapon-affine object fold continuously on subsequent
frames. The normal particle path remains shot-gated, so ambient flames and
structural geometry do not regain continuous access to the weapon transform.
The first observed laser frame can remain native while its layout is learned;
all following frames use the gun transform. Runtime confirmation is `VRPose
laser: learned continuous beam input layout`.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`9A3AD95B7AFB09CA0FA6DC3799770275E6EC7D737F4E8B38439C0C173F860E18`.

**Headset validation (2026-08-18): fixed.** The laser beam now follows the gun
through controller translation and rotation rather than following HMD yaw.
The native face-locked clock remains removed. This validates the targeted
continuous-laser exemption layered on top of the shot-gated generic particle
fold.

Future-maintenance rules from this regression:

- Keep the generic particle/object weapon-affine fold shot-gated. Removing or
  broadly relaxing that guard reintroduces the ambient flame and structural
  building attachment bugs.
- A persistent weapon effect must receive an effect-specific exemption based
  on a captured render signature; proximity, index count, or particle layout
  alone are not safe discriminators.
- The validated beam signature is a 24-index `DrawIndexed` with VS
  `6C14CA9DCD06FE85` and PS `339689302CA37E17`.
- Metro binds the beam input layout before mapping its object buffer, but binds
  its shaders only afterward. Consequently the implementation learns the
  layout from the completed beam draw and uses it beginning with the following
  frame. Do not try to identify the pending object-map solely from the current
  shader handles; at that point they still describe the preceding draw.
- Preserve the watch UI suppression independently: exact watch-icon anchor
  followed by five immediate generic-text glyph draws (`HH:MM`) in an
  eight-draw window. Do not route those glyphs through the disabled custom
  wrist transform.

### 5.16 Scripted-camera pitch pass-through and injection gating — 2026-08-17

Follows the camera-pitch repair in `Notes/24`. That repair closed the culling
follow's pitch loop on Metro's **real** camera (`ReadCameraVec`) instead of on
an accumulator of what we assumed the engine applied. Everything below is
consequence of that change, good and bad.

#### The pass-through and why it is gated

`GetScriptedPitchOffset()` is the **only** consumer of scripted-camera
detection. It feeds `gamePitch` in the additive view build, letting a scripted
camera's pitch show through so cutscenes frame as authored. Every false
positive is therefore felt directly as *the world moving vertically with the
player's head during ordinary gameplay*.

Detection by inference could not be made reliable. Recoil, follow-loop lag and
a genuine cutscene all present as "engine pitch diverges from HMD pitch". The
recoil ignore window (§5.8) narrowed this but did not close it.

Two settings now control it, both in `VRPose.cpp`:

| Constant | Value | Meaning |
|---|---|---|
| `kScriptedPitchPassthrough` | `true` | Master switch. Set `false` to disable cinematic pitch entirely — removes the failure mode outright, at the cost of the camera never pitching for you (normal for VR mods). Verified to stop the symptom completely. |
| `kScriptPitchMin` | 0.21 rad (~12°) | Divergence needed to start the timer |
| `kScriptSustainFrames` | 72 (~1 s) | How long it must hold in the marginal band |
| `kScriptPitchFast` | 0.52 rad (~30°) | Above this, engage almost immediately |
| `kScriptSustainFast` | 6 (~85 ms) | Fast-tier wait — needed so a creature pinning the player snaps the camera to it |
| `kScriptPitchDrop` | 0.07 rad (~4°) | Release, resets the timer |

**Why duration rather than magnitude alone.** The earlier logs show the bands
overlap: genuine scripted cameras latched at +14° to +72°, but so did
ordinary-play events at +4.1°, +5.6° and +6.3°. No single-frame threshold
separates those. Duration does — recoil is a ~250 ms kick with its own ignore
window, and follow-loop lag closes within a few frames now that the loop
measures the engine. Neither holds a large divergence for a second; a scripted
camera does nothing else. The fast tier exists because 30°+ is a band nothing
in ordinary play reaches, so it does not need the long guard.

Player-confirmed with the gate in place: gunfire and head movement no longer
move the world, and scripted events frame correctly.

#### Anti-windup — the closed loop does not stop on its own

The open-loop version had an accidental brake. It compared the head against its
**own accumulator**, so every command advanced the thing it was measuring, the
error closed within a few frames, and it went quiet — *even when Metro was
ignoring the input entirely*. Measuring the real camera removed that brake: if
the camera does not move, the error never closes and the loop commands mouse
`dy` every frame indefinitely.

Live consequences, both traced to this and both **new since the tilt fix**:

- the mouse cursor drifting down menu screens, tracking head pitch;
- Metro's button prompts flipping between keyboard and controller, because each
  synthetic MOUSE event tells the engine the player switched to mouse while the
  virtual pad reports a controller.

The existing `sPauseMenuExpected` guard is inferred from START/B presses only,
so any menu reached another way is unprotected.

**Attempt 1 (failed).** Suppress after ~20 dead frames, probe once every 30
frames to detect the engine listening again. The clear condition was wrong — it
cleared whenever the last command was zero, which once suppressed is every
frame. It suppressed, immediately un-suppressed, commanded, went dead, and
repeated. This converted continuous drift into **stepping**, which matches the
player's report of the cursor moving down in increments and the prompts
switching at the same rate.

**Attempt 2 (current, still not sufficient).** Clear the dead counter only on
real camera movement (`kCameraAlive`, ~0.11°/frame); hold the verdict when we
are quiet *and* the camera is still; suppress absolutely with no probes.
Resumption relies on camera liveness from any source, on the reasoning that a
menu freezes the camera outright while gameplay never leaves it perfectly still
(stick, walking, bob, sway).

**Status: the cursor still walks down the merchant screen.** So either
suppression is not engaging there, or something other than this loop is moving
the cursor. The `VRPose cullpitch: injection SUPPRESSED/resumed` log line with
its dead-frame count distinguishes those two and should be read before any
further change.

#### Do not repeat

- Do not re-add the stall detector as a **controller** (`Notes/24` §4).
- Do not add another per-frame motion threshold to separate scripted from
  ordinary play; the bands provably overlap.
- Do not resume the scripted-state flag hunt as it stands (`Notes/24` §5).

#### Pitch-follow necessity test — 2026-08-17, CONCLUSIVE

`kPitchFollowEnabled = false` (pitch mouse injection disabled, everything else
including the measured cancellation left intact) was tested live to see whether
the CPU occlusion bypass at `+0x826B50` had made the pitch follow redundant.

**It has not. The follow is still required.** With it disabled:

| Observed | Meaning |
|---|---|
| Things pop in and out much more | Engine still culls against its own camera |
| Very hard to pick things up, even crouching | Interaction ray follows the engine camera |
| Laser sight and muzzle flash no longer attached to the gun correctly | Those are positioned against the engine camera too |

Restored to `true` immediately.

**But the test was still decisive, because it isolated blame.** With pitch
injection off:

| Symptom | Result |
|---|---|
| Hitching / twitching during scripted events | **STOPPED** |
| Merchant mouse cursor walking down the screen | **STOPPED** |
| Keyboard/controller button-prompt flipping | **Continued** — the YAW injection still sends mouse `dx` every frame |

So all three are caused by **synthetic mouse input**, not by the follow itself,
and the prompt flipping is specifically attributable to the yaw half.

**Conclusion: keep the follow, replace how it is DELIVERED.** Steering Metro's
camera through the input system is the shared root of the scripted tug-of-war,
the menu cursor, the prompt flipping, the anti-windup guard, the approximate
`kPitchResponse` and the lag compensation. Writing the camera directly — the
setter at `metro.exe+0x7E09EC`, Notes/14 Stage 3 — removes all of them at once
and is exact by construction.

Known cost of that rework, not yet done: `RedirectShot` reads the camera's
forward vector from the same object (`+0x170`) for `baseYaw`, so the shooting
pipeline and the sight zeroing need re-validation; the matrix is replayed from a
command stream, so the hook must land at a stage that is not overwritten; and
the camera object also feeds culling, interaction and the weapon transform.

#### Scripted hitching — fixed by yielding the injection, 2026-08-17

The hitching during scripted events was the pitch injection fighting the
script. Fixed by yielding: if the loop has commanded for ~0.6 s with the error
not closing, something else owns the camera, so stop pushing. A probe every 20
frames detects control returning.

Safe in a way the reverted stall detector was not: it touches `dy` only. The
view's scripted pass-through is gated separately on sustained divergence, so a
mis-fire costs a little culling lag and nothing visible.

**First version false-fired 19 times in 20.** It asked whether the error had
shrunk over 0.6 s, but while the player moves their head the target moves too,
so the error is continually re-opened and a healthy loop looks like a losing
one. Measured: 19 yields between 3.4° and 10.4° during ordinary play, all
releasing moments later with "error closed"; exactly one genuine scripted
camera, at 42°.

Each false yield stopped the injection long enough for the engine camera to
fall behind the head — felt as the world following the headset, and as
camera-referenced effects shifting.

Tightened to: error > ~15° (`kFightErr = 0.26f`) **and** the head must be
still (`< ~0.23°/frame`) before "no progress" counts at all.

**Not caused by this:** decals/graffiti/room numbers moving with the RIGHT
CONTROLLER are §5.15 (weapon-instance substitution gate), not camera lag.
Camera lag slides decals along a surface; §5.15 sticks them to the hand.

**Still open (pre-existing):** the world follows the head when moving up/down
FAST. Lead: the view may be built from a head pose sampled earlier in the frame,
so high angular speed shows a stale pose. Late-latch machinery already exists
(`kPortalCameraLateLatchBlend`) — check whether the view path uses it.

#### Session end state — flags as actually set in source

Verified by reading the tree, not from memory:

| Flag | Value | Meaning |
|---|---|---|
| `kAdditiveViewBuild` | `true` | View assembled as yaw+yaw, pitch+pitch, roll from headset |
| `kBodyPitchFromEngine` | `false` | Body contributes no pitch; head only |
| `kLevelWeaponBodyFrame` | `true` | Weapon body frame is yaw-only |
| `kPitchFollowEnabled` | `true` | Pitch injection ON — **required**, proven by disabling it |
| `kScriptedPitchPassthrough` | `true` | Cinematic pitch on, behind the sustained gate |
| `kControlStateSamplerEnabled` | **`false`** | F7/F8 ground-truth sampler is OFF |
| `kHuntEnabled` / `kWatchEnabled` | `false` | Differential scan and shortlist watch retired |
| `kReverseEngineeringDiagnosticsEnabled` | `false` | F5/F6/F9/F12 probes off |
| `kRetiredCameraProbesEnabled` | `false` | — |

Note the sampler is **off**, contrary to an earlier handoff note that described
it as enabled. It will not produce `vr_control_state_candidates.txt` in this
state, and that file did not exist as of this session. Re-arm deliberately if
that hunt is resumed — but see `Notes/24` §5 first.

Still-active log-only diagnostics (no behavioural effect, remove when done):
`uiplace` (UI canvas centre in NDC), `yawresp` (engine yaw response vs
commanded), `cullpitch`, `VRPose script:` pass-through transitions.

Safety net: `d3d11.known-good-2026-08-17.dll` sits beside `d3d11.dll` in the
game folder — a byte-exact copy of the working build. Rename it over
`d3d11.dll` to revert without rebuilding.

#### Fast head movement dragging the world — pose staleness ELIMINATED, 2026-08-18

Symptom: moving the head up/down fast, or in rapid succession, makes the world
follow for a moment. Pre-existing; not introduced by the camera-pitch work.

Leading hypothesis was pose age — the head pose is latched at Present while the
view matrix is written during the next frame's rendering. Measured directly
(`LogPoseStaleness`, log-only): at the moment the view matrix is built, re-poll
the headset and compare that pitch against the pose the view is actually being
built from.

**Result: worst drift 0.06°, the overwhelming majority exactly 0.00° over 1534
samples, including deliberate fast head movement.** The pose feeding the view is
current. Pose age is NOT the cause and a late-latch would fix nothing — worth
knowing before touching the submit path, which is adjacent to §5.5.

Also eliminated by measurement in the same session: the scripted pass-through is
not involved. It engaged **twice** in the whole session — one real scripted
camera at +104°, released two frames later.

That leaves the stereo presentation path as the leading candidate, i.e. §5.5
(AER left-eye jitter: "the world still doubles/jitters in the physical left eye
during head movement"). Same provocation — head movement — and that issue
already carries an extensive eliminated-causes list. Treat this as evidence for
§5.5 rather than as a separate problem, and add to that issue's record that pose
age at view-build time is now ruled out.

### 5.17 Magnifying scopes, muzzle flash, laser beam and projected dot — 2026-08-19

Full scope-attachment census and magnification details are in `Notes/29`.
Magnification is attachment-driven rather than weapon-driven: unscoped weapons
do not zoom, and known 2× scope mesh signatures trigger correctly. The passive
census remains the path for adding a newly encountered scope variant.

Full muzzle/laser investigation and current constants are in `Notes/30`.

Current results:

- visible laser beam follows the weapon through physical and thumbstick turns;
- both muzzle-flash components remain on the barrel, including trigger release;
- a custom stereo red dot is drawn at the canonical bullet-aligned aim point;
- its size is distance-sensitive and its brighter/sharper appearance is close to
  the native mark;
- filtered beam animation adds some walking sway while preserving exact resting
  aim alignment;
- running sway still does not travel as far sideways as the visible beam and is
  accepted for now;
- the original HMD-locked native red dot is still visible and unresolved.

Do not return to absolute beam-endpoint positioning for the custom dot. The
projector-local point put it beside the beam, Z=0.432 put it midway down the
beam, and the verified mesh tip `(0,0,1)` still did not match bullet impacts.
The damage ray and visual sway ray differ. The current implementation therefore
anchors to `GetReticleDirection()` and overlays only the high-pass changing
component of beam motion.

Last deployed build SHA-256:
`DBF42B2001E9307C39EFB488BCB518D9593E133A6C908731A1733975CC5D5C7D`.

**Shared laser-layout wall-decal regression (2026-08-20, active test):**
`Video Project 43.mp4` shows the yellow projected wall markings around the
opening ladder following the right controller. The continuous-laser exemption
was still being applied at object-buffer map time using only the input-layout
pointer learned from the exact beam draw. That layout is not beam-specific;
Metro also uses it for projected/static wall decals. Once a laser-equipped gun
had been seen, any nearby object using the shared layout could continuously
inherit the weapon affine. This is the same class of unsafe discriminator as
the earlier broad particle-layout fold, despite the beam draw used to learn it
being exact.

The shared-layout exemption has been removed completely. The beam is now
corrected at draw time, where its complete captured signature is available:
24-index `DrawIndexed`, VS `6C14CA9DCD06FE85`, PS
`339689302CA37E17`. The live-eye private cb0 applies the weapon affine to the
beam's current `V*W`; the recorded second-eye replay rebuilds `V_right*W` and
applies the same affine before recomputing WVP. The resulting corrected beam
pose continues to feed the custom projected-dot path. Generic map-time
particles remain gated only by confirmed muzzle-effect activity, so wall
decals, ambient flames, and structural geometry have no continuous path to the
weapon transform. The retired laser-dot particle census was also disabled.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`FB404EB0E98ABA104BB14E5A21A175E96B711877CBD0E8A6BF061A6CC358CAD0`.

Required validation: revisit the opening ladder and move/rotate the right
controller while watching the yellow wall decals; then verify that the visible
laser beam and custom red dot still follow the gun in both eyes. Also spot-check
the previously fixed ambient flame/building locations if convenient.

Headset validation confirmed that removing the shared-layout exemption fixed
all of the reported wall decals, but the replacement produced two visibly
separated laser beams, one per eye. The cause was a draw-kind mismatch in the
second-eye replay: the new correction was nested under `kind == 2`
(`DrawIndexedInstanced`), inherited from the muzzle-particle path, while the
exact laser is `kind == 0` (`DrawIndexed`). The live left-eye draw therefore
received its private corrected cb0, but the recorded right-eye draw bypassed
the new beam correction.

The replay now accepts `kind == 0` only when the exact 24-index laser shader
signature matches; the muzzle branch remains restricted to its instanced draw.
This restores a corrected per-eye beam without reopening any input-layout path
for decals. Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`09CB4A009602E90D51032A7C228F73E61386D8D04AA9583F143AF162C40B5E99`.

Required validation: confirm that the two eye images fuse into one laser again
and that the ladder-area wall decals remain fixed to the walls.

The draw-kind correction removed the completely native right-eye copy, but the
player's side-by-side screenshot (`Screenshot 2026-08-20 001600.png`) still
showed a clear inter-eye mismatch. Reconstructing the second eye as
`M * (V_right * W)` was not equivalent to the last headset-validated beam
behavior. The beam is a camera-authored view effect; the working map-time fold
placed one corrected beam matrix in the tracked buffer and the stereo replay
reused that same result, rather than giving the effect physical IPD disparity.

The exact-signature draw-time path now reproduces that behavior deliberately:
the live draw records its final corrected WV, and the second-eye replay copies
that WV and its left-eye WVP rather than independently rebuilding a right-eye
beam. Classification remains the exact 24-index/shader signature, so the
shared-layout decal regression stays closed. Release x64 built successfully
and was deployed with matching source/destination SHA-256:
`2285B4F0CA2D0322E6A32D965A94F4EDAD44B2D7328EAA2DA7F8B39C4847339A`.

Required validation remains laser fusion/alignment plus stationary wall decals.

Isolation result: with only the exact 24-index beam mesh suppressed, the player
reported that no laser beams were visible. The custom marker remained enabled,
so the stereo mismatch is conclusively in the beam mesh rather than
`DrawVRLaserDot` or the native-projector suppression. Temporary beam suppression
was then removed.

The remaining implementation difference from the original validated layout
fold was the source object payload. The first draw-time versions reconstructed
WV from cached object/world and generic eye-view matrices. Metro's stereo path
already retains the complete finished `eye0Bytes` and `eye1Bytes` produced at
map time, including corrections not represented by that reconstruction. The
live exact-signature beam now copies the full saved eye-0 cb0, folds the weapon
affine into its existing WV, preserves the rest of the payload, and records
that affine. The replay folds that same captured affine into the full saved
eye-1 WV before rebuilding its WVP. This is the draw-time equivalent of the
old headset-validated per-eye map fold while keeping all decal-shared layouts
out of the classifier.

Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`B14CC6DBD3EA0A83D23560DC97115BD058797E6FDB2E861ECB6800A6899514CA`.

Required validation: beam visible and fused/attached in both eyes; decals remain
fixed.

That full-payload/private-cb build still showed the same mismatch. The remaining
structural difference was the buffer identity itself: both eyes' beam draws were
still bound to a private cb0, while Metro's established stereo machinery swaps
only the tracked game cb0. The exact beam handler now folds the captured weapon
affine directly into both completed tracked byte arrays immediately before the
draw, writes corrected eye 0 into the currently bound game buffer, and lets
`HoldForSecondEye`/`BeginTwinPass` consume the already-corrected eye-1 bytes by
their normal route. The draw is still admitted only by the exact index/shader
signature; the decal-shared input layout is not consulted.

Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`12D421BBC86E53733D2E730357E8C0420A480F656383DD78EF237E0384020D1F`.

Headset result: the tracked-buffer route fixed the stereo split and produced one
beam. A pose-space error remained: on level entry the beam sat to the right of
the gun, then swept left/right during a physical 360-degree turn. This matches
the documented duplicated scene/body correction failure mode. The exact beam
now receives one additional pre-cancellation using the recorded first-eye view
correction before its affine is folded into the tracked payload. This is
laser-only; weapon meshes and muzzle effects retain their validated transform.

Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`823AF22DC139F9A6F13B7FAC4D9AE7096BA03DF7AAE87BA92EFAFC80FB1DB686`.

Required validation: initial lateral alignment and physical full-turn stability;
also ensure the beam remains single and the wall decals remain stationary.

Full extra cancellation overcorrected: the beam began left of the gun and still
swept during physical turning, whereas no extra cancellation began right. The
needed transform is therefore bracketed between those endpoints. The laser-only
path now applies a half rigid correction: rotation uses the shortest-arc unit-
quaternion square root (rather than a linear matrix blend that becomes singular
near 180 degrees), with half translation, then pre-cancels the beam affine by
that result. Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`D5FB14D56EC3FC89455A41FC9B24E0F23980BCE4015ADD11787777188789C968`.

`Video Project 44.mp4` showed that the half correction did not address the
actual failure. At comparable gun poses, the visible beam crossed from far left
of the muzzle to far right during the physical turn. This is not a small
residual correction or fixed offset; Metro's camera-authored WV is the wrong
base pose for a controller-attached beam.

The exact tracked-payload path now discards the native beam pose completely.
It obtains the gun's published rendered view position and the canonical barrel
direction shared by redirected bullets and the custom red dot, then rebuilds
the beam's local frame with Z along that ray. Only the native mesh's X/Y
thickness and Z length are retained. The completed left-eye pose is transported
rigidly through `V_right * inverse(V_left)` for eye 1, preserving the validated
single-beam stereo path without any camera/body term. The decal-shared input
layout remains unused.

Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`01252C3E926189203A54182C31992052DF5DE83D71E83EB1BF9D211747CF4DF3`.

Required validation: beam origin/alignment at the gun, stability through a
physical full turn, one fused beam, and stationary wall decals.

Headset result: direct reconstruction was worse—two eye-separated beams, a
left-of-gun origin, and the same physical-turn sweep—so it was removed. The
important difference from the original easy/working fix is timing. The old
layout path called `BuildWeaponAffine` at cb0 map time; the safe exact-signature
path cannot classify the draw until later. Metro can refresh the weapon
instance between those points, so applying the later affine is not equivalent
even when the saved eye matrices are identical.

`PatchMappedVRObjectData` now passively snapshots the weapon affine alongside
each object's WV at its original map boundary. When the later draw matches the
exact 24-index beam shader signature, that map-time affine is folded into the
already-completed tracked eye-0 and eye-1 payloads. This recreates the original
working timing and matrix math while still never admitting an object merely
because it uses the decal-shared input layout. The failed direct pose and half-
correction behaviors are inactive.

Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`F141A38AAE759E2A7B2959B528D82EDB5B5D66FF411C3A2F689BE7DEE27A4823`.

Correction to the preceding entry: the map-affine draw-time build was **not**
an actual restoration of the last working laser implementation, despite being
described that way. Headset testing still showed the physical-turn sweep. The
player correctly called out that discrepancy.

The laser code has now been restored byte-for-byte in behavior against the
last source revision where the beam was headset-validated: learned beam input
layout, continuous fold inside `PatchMappedVRObjectData`, original per-eye map
timing, original projector-pose capture, no laser draw-time private/tracked cb0
replacement, and no special laser replay branch. The only laser-related source
difference is that the retired one-shot impact-particle dump remains disabled;
it has no rendering effect. All direct reconstruction, half/full cancellation,
map-affine draw-time, and tracked draw-time experiments are removed.

This intentionally restores the known-good laser baseline before attempting a
different decal exclusion. Because the original classifier is the shared input
layout, the wall decals may again follow the controller in this baseline; that
tradeoff is explicit and expected for this validation build.

Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`271AA572FB51ABE0558B719B74C76E6A62359D4103B21E2A5E52F4EA125CA623`.

Player validation confirmed that this restored the laser to normal. The next
decal-only exclusion keeps that exact map-time beam transform and changes only
its classifier. The exact 24-index beam draw now learns its vertex-buffer and
index-buffer identities alongside its input layout. At cb0 map time, the
continuous-laser fold requires all three identities to match. The wall decals
reuse the beam's input layout but use different mesh buffers, so they should no
longer inherit the controller affine. A bounded mismatch diagnostic records
current versus learned buffer pointers if Metro binds the beam geometry later
than expected; it has no rendering effect.

Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`664F54929CCE56CA8C8D1981FC9A010CD53E1041BF07B01752A741249C635992`.

Required validation: laser remains in its confirmed normal state, and the early
wall decals remain stationary when the right controller approaches them.

Headset result: the decals still moved. The runtime diagnostic recorded the
beam geometry once but no shared-layout buffer mismatches at all. Metro leaves
the beam's IA vertex/index state bound while it maps later objects, so those
pointers are stale in exactly the same way as the input layout and cannot
classify the upcoming draw. The buffer-qualified test was removed.

The replacement classifier learns the beam object's transform shape at the
exact beam draw. Every object cb0 map records the X/Y column-length squares of
its unchanged world matrix; the exact 24-index beam draw adopts the immediately
preceding values. On later frames the original, validated map-time controller
fold requires the shared layout plus matching transverse scales within 5%.
Position, orientation, and Z length remain unrestricted, so aiming and changing
beam hit distance do not affect admission. Larger decal quads should fail the
scale match. Bounded logs record candidate and learned scales for validation.

Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`7B3B6E247A70816C8EDB9138B7875852F89A9BF5C2F263A96C5B4AD0D40F43CE`.

Required validation remains: laser alignment/stability through physical turns,
then approach the early wall decals with the right controller.

Headset result: decals still moved. The captured candidates show why: the beam
and nearly every affected shared-layout object write an identity world basis
(`x^2 ~= 1`, `y^2 ~= 1`). Their vertices are already authored in world space,
so cb0 world-scale/shape cannot distinguish the decal from the beam. The scale
classifier was removed.

The next classifier uses render-sequence context available at the same proven
map boundary. Each object map snapshots the vertex/pixel shader pair left bound
by the preceding draw. The exact beam draw learns the pair from its immediately
preceding map; later continuous-laser folds require the shared layout and that
predecessor pair. This does not change beam matrices, map timing, or replay.
Bounded logs record current versus learned predecessor handles.

Release x64 built successfully and was deployed with matching
source/destination SHA-256:
`BAE337EDA2EDB8FBD3B6DAD3B458FB402CD06B12E5B1F0C8F21AF2DBB4E6637A`.

Required validation remains the laser followed by the early wall decals.

**Headset-confirmed fixed:** the weapon laser remains correctly attached,
single/fused between the eyes, and stable during physical turning; the early
wall decals remain fixed to the world when the right controller approaches.
`BAE337EDA2EDB8FBD3B6DAD3B458FB402CD06B12E5B1F0C8F21AF2DBB4E6637A` is the
validated laser-plus-decal baseline.

Final implementation and future guardrails:

- Keep the original laser correction in `PatchMappedVRObjectData`. Its per-eye
  map timing, weapon-affine fold, and projector-pose capture are headset-proven.
- Identify the continuous beam by the learned beam input layout **and** the
  learned predecessor VS/PS pair. The exact 24-index beam draw teaches that
  predecessor context from the immediately preceding object map.
- Do not return to input-layout-only classification: decals share the layout.
- Do not use IA vertex/index buffer identity at object-map time: Metro leaves
  the beam's bindings stale while mapping later objects, so decals appear to
  have the same buffers there.
- Do not use cb0 world-transform scale/shape: the beam and affected decals both
  commonly use identity world bases because their vertices are already authored
  in world space.
- Do not move the beam correction to draw time or reconstruct its pose there.
  Those experiments caused stereo splitting, lateral offsets, missing beams,
  and HMD-turn sweep even when they used the exact beam draw signature.
- The bounded `VRPose laser sequence` diagnostics were log-only and were
  removed in the 2026-09-01 follow-up below.

**Late-level laser attachment and native-dot follow-up — 2026-09-01:**
`Video Project 61.mp4` exposed two remaining failures. The beam's map-time
classifier stored only one predecessor VS/PS pair. Metro cycles among several
valid predecessor contexts in later scenes, so each newly observed pair replaced
the previous valid one; the next map then missed the weapon-affine fold and left
the beam on the camera. The exact beam draw now accumulates a bounded set of all
positively observed predecessor pairs. Map-time correction still requires the
validated shared layout plus an exact learned pair, preserving the wall-decal
exclusion while allowing Metro's legitimate render-sequence variants.

The native projected dot was already positively identified and marked skipped,
but `BeforeDraw` continued through shader-override processing afterward. A
draw-from-caller override can submit during that processing, before the final
null-pixel-shader safeguard. The replacement-dot path now returns immediately
after marking the native projector skipped and drawing the custom stereo dot, so
the native draw cannot reach any later submission path. The obsolete predecessor,
sequence, and projected-dot diagnostics were removed.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`7C123C06CED6324EB7426265BF670A6A6519A3DB5451F3DB5AA14E18870EF485`.

Required headset validation: move the laser-equipped gun well away from the
view centre while turning and walking through the late outdoor sequence. Confirm
that only one impact dot remains, that the beam never falls back to the head,
and that the previously protected wall decals remain fixed to the world.

Follow-up from `2026-09-01 10-44-29.mp4`: the player beam remained attached to
the weapon and no decal regression was observed, but the native red spot was
still present. Its apparent radius decreased as the surface approached and then
vanished at close range, which identifies it as Metro's perspective deferred
light projector rather than the constant-screen-size custom marker. The video
also showed an NPC laser moving with the player's head.

The exact 24-index beam signature and projected-light signature are shared by
player and NPC lasers. The map-time beam correction now additionally requires
the beam's near endpoint to lie in the near-eye player-viewmodel region. Only
such a positively owned player beam can teach predecessor contexts, refresh the
player-laser frame latch, or receive the weapon-affine fold. Nearby NPC beams
therefore retain their world transform instead of inheriting the player's gun
or head transform.

Projected-dot replacement now has a corresponding ownership gate: the captured
light source must lie within the player's near-eye region. The skip no longer
depends on one tracked VS cb0 buffer identity or private corrected constant
buffers, because the native projector is replaced rather than rendered. This
allows alternate/replayed player-projector submissions to be suppressed while
leaving NPC projectors intact. No new runtime diagnostics were added.

Release x64 built with 0 warnings and 0 errors and was deployed with matching
source/destination SHA-256:
`3CBEF693EA9D5E99D90065DEBB600D41FACA8F0F423E5372D9A941C6D849A117`.

Required headset validation: verify that the player's beam remains attached to
the gun, the original shrinking native spot is absent at both long and close
range, the custom dot remains, the NPC laser stays fixed to its NPC/world-space
muzzle while the player turns their head, and nearby wall decals remain fixed.

First ownership-gate test result: the NPC laser correctly stopped following the
player's head, but the player's beam became head-relative and the original
shrinking spot remained. Both regressions had one cause. RenderDoc event 929
shows that the beam mesh spans local Z `[0,1]`; its muzzle/base is therefore the
transform origin. The first gate incorrectly tested local Z `-1`. Since the Z
basis stretches to the contacted surface, that fabricated point moved farther
away as beam length increased and rejected the player's long beam. Rejection
also prevented `sLaserBeamLastFrame` from refreshing, so native-projector
replacement never became eligible.

Player ownership now tests the actual transform origin within the near-eye
viewmodel region. This remains independent of beam length and continues to
exclude NPC world-space muzzle origins. Release x64 built with 0 warnings and
0 errors and was deployed with matching source/destination SHA-256:
`5EFE29E767BD35C7355DA07918BBC929F8C5E15F27F298ED1C473C4C536E1015`.

Required headset validation remains the player beam, shrinking native spot,
custom marker, NPC laser during head turns, and nearby stationary wall decals.

Subsequent isolation tests confirmed that the remaining head-relative point was
not the already identified 36-index deferred projector. With that projector and
the custom marker disabled, the head point remained while the gun point
disappeared; hiding the beam did not affect it. Suppressing the later 480-index
light-volume draw also did not remove it.

An automatic render-target frame analysis captured the point's first pixel
contribution. It is a separate `DrawIndexed(96)` layer using vertex shader
`98CD6A5E2980BAE1` and pixel shader `67F4CD3569D152D4`. The later 480-index pass
only broadens/composites lighting around that already present point. The new
suppression therefore requires the exact 96-index shader pair and a fresh,
positively owned player-laser frame; unrelated scene lights and NPC lasers are
left untouched. The known 36-index projector is again replaced by the custom
gun-authored marker, and the corrected native beam remains enabled.

All automatic capture and hunting options were disabled after identification;
no active capture or temporary draw diagnostic remains. Release x64 built with
0 warnings and 0 errors and was deployed with matching source/destination
SHA-256:
`E49F138803FF809D0A39C1C2D1F87FB7A8945CD7445EF2929410EB5B291AD776`.

Required headset validation: confirm that the custom gun dot and gun-attached
beam are present, the head-relative native point is absent, NPC lasers remain
fixed to their owners during head movement, and world lighting/decals remain
unchanged.

Final headset validation: fixed. The exact 96-index suppression removed the
remaining head-relative native point while retaining the custom gun dot and the
gun-attached beam. Earlier validation in the same sequence confirmed that NPC
lasers remain attached to their NPC/world-space owners and that the protected
wall decals were unaffected. This is the release configuration merged below.

Follow-up from `Laser Issue.mp4`: lateral head movement still made the visible
laser endpoint slide across nearby geometry, and one transient sequence made
the laser appear fully head-attached until Metro was restarted. The surviving
motion came from the replacement marker itself: when no fresh corrected beam
pose was available it fell back to an infinite-distance, head-centred direction.
Even while a beam pose was fresh, the marker retained only a filtered delta
from its finite endpoint, so tracked head translation did not receive exact
surface parallax.

Current test candidate removes that fallback. The corrected beam keeps Metro's
finite traced length and weapon-folded base but replaces its camera-authored Z
axis with the calibrated controller/barrel direction shared by projectiles and
the reticle. The custom marker projects that exact finite beam tip into both
eyes. If a fresh corrected beam is unavailable, the marker is omitted for that
frame instead of drawing a wrong head-centred point. Learned beam-predecessor
identity is now stored by stable shader hashes rather than transient shader
object pointers, preventing level/render-object recreation from discarding an
otherwise known sequence.

Release x64 built successfully (one pre-existing `Override.h` dominance
warning, zero errors) and was deployed with matching source/destination
SHA-256:
`A6E2B9548B9C0659FCDA0B5034B2137EA3E9CE9A7F85A94E234DECD2BAD09BA0`.

Required headset validation: hold the controller still while moving the head
left/right near a wall and confirm the beam and dot stay on one finite surface
point; then cycle weapons and continue through the level to check that neither
ever becomes head-attached. Also recheck NPC lasers and stationary decals.

Headset validation: the finite-endpoint candidate is substantially more stable
under lateral head movement and is accepted for release. No recurrence of the
fully head-attached state was observed during this test. The branch remains
available for any later edge-case follow-up.

### 5.18 Alternating broken reloads — fixed 2026-08-19

After the first successful save load, successive checkpoint reloads or
main-menu save loads alternated between good and broken gameplay.  In a broken
load, physical turning blurred the view and exposed missing geometry, movement
remained relative to the old heading, and objects popped in only at close
range.  Opening the pause menu and backing out with B restored everything.

Cause: the mod inferred pause state by toggling `sPauseMenuExpected` on each
synthetic Start rising edge.  Selecting Reload or loading from the main menu
dismisses/replaces Metro's menu without a matching Start/B edge, leaving that
flag set.  `UpdateCullingFollow()` then returned every frame, so Metro's engine
camera stopped following HMD yaw.  The next Start press toggled the stale flag
off, producing the exact bad/good reload alternation.  The stale engine camera
also explains the whole symptom cluster: Metro interprets WASD relative to it
and performs CPU visibility culling against it.

First fix attempt: `NotifyGameplayHUDConfirmed()` cleared
`sPauseMenuExpected`, but headset validation failed because the optional ammo
magazine marker did not appear in the affected checkpoint.  The recovery log
never fired.  Do not rely on that marker as the only post-load boundary.

Current fix: the renderer now publishes the exact loading-panel shader
`D924AAD8F41ECCE2`.  If it appears while the pause latch is set, recovery arms;
the latch remains set throughout loading and is cleared after the panel has
been absent for several frames.  This directly covers checkpoint/save loads
without allowing culling-follow mouse input into a normally rendering pause
menu.  The transition log reads `VRPose input: loading screen exited - cleared
stale pause-menu state`; arming logs `VRPose input: loading screen seen with
pause latch - recovery armed`.

The first attempted build had SHA-256
`501336EBCFA44C6A49D5ECE8131086A3B949D0425B0BED40881249343CE3D097` and did
not fix the issue.

The loading-panel replacement (including explicit arm/clear diagnostics) built
successfully and deployed with matching source/destination SHA-256:
`4B2641C641546C01B387E6D36A45C704A578A44A8DE6A06D55B45470E82B8CFF`.

### 5.19 Analog thumbstick locomotion — 2026-08-19

Normal VR-controller locomotion now publishes the movement controller's raw
direction and magnitude through the synthetic Xbox left stick instead of
thresholding it into W/A/S/D. A radial 0.35 deadzone is rescaled to the full
0..1 output range, preserving gradual walking speed and true diagonals without
an abrupt 35% speed step. The virtual right stick remains zero so Metro's
gamepad look cannot compete with head/weapon steering. All four former movement
keys are explicitly released on the analog path, and the existing 100 ms XInput
watchdog zeros locomotion if VR updates stall. Adjustment modes and the VR menu
continue to publish a neutral left stick.

Release x64 built and deployed with matching source/destination SHA-256:
`D1B156F4D6CAE234A923F198A3CA3FC30CD0BD64C3A9F51D7283EC2E9D771D46`.

First headset validation confirmed two benefits: controller prompts no longer
fall back to keyboard glyphs nearly as often, and sprint activation is much
more reliable. It also exposed retained movement after releasing the stick.
The likely source is OpenVR legacy Axis0 behavior: some controller profiles
retain the last axis coordinates after touch is released. The movement path now
learns whether Axis0 capacitive touch is supported; after touch has been seen,
an untouched stick/pad is authoritative neutral and forces both published axes
to zero. Profiles without touch sensing retain radial-deadzone behavior, so the
fix does not disable their movement.

Touch-aware neutral build deployed with matching source/destination SHA-256:
`11C826BEDFF7202D7918FE82CC2C633E4CC0F97D88D51E7D79B7CEC65E530A58`.

Headset validation: the touch-aware neutral did **not** stop retained movement,
so stale OpenVR coordinates were not the cause. The more likely mechanism is
Metro's mixed-device arbitration: it accepts non-zero XInput locomotion, then a
synthetic culling-follow mouse event switches the active device; the subsequent
zero controller sample does not release the already-latched movement action.
The input path now detects a non-zero-to-zero published-stick transition and
withholds culling-follow mouse injection for 50 ms, leaving several controller-
only polls in which Metro can consume neutral before mouse steering resumes.
Bounded diagnostics record both publication of the release and delivery of the
neutral XInput packet if another headset run is needed.

The same analog-axis/menu interaction made pause navigation harder. During the
startup/main menu or an expected pause menu, the virtual left stick is now held
neutral and the physical movement stick is converted to dominant-axis Xbox
D-pad buttons. Gameplay remains fully analog, and menu input remains on the
controller path so it should not bring back keyboard glyph prompts.

Neutral-yield/menu-D-pad test build deployed with matching source/destination
SHA-256: `8CC6147D20D9660F9BA7E79E9338EB332D03627250782184119A4EF9C7AFFB89`.

Validation: D-pad menu navigation is substantially better, but movement still
latched. The bounded diagnostics made the boundary conclusive: every tested
release logged both `left stick released` with a non-zero prior value and
`XInput delivered neutral` with an advanced packet number. OpenVR, analog
processing, shared publication, the watchdog, and the hooked XInput result are
therefore all releasing correctly; retention occurs inside Metro after input-
device arbitration. The next build keeps analog XInput while held, but on its
non-zero-to-zero edge also sends unconditional W/A/S/D key-up events. These give
Metro's now-active keyboard/mouse movement actions an explicit stop without
adding keyboard key-downs or sacrificing analog speed while moving.

Explicit keyboard-break test build deployed with matching source/destination
SHA-256: `890FB49866B1618FBDDA6C93F2215645C7EA949BD92E5A73394D6A7FAB567895`.

Validation: explicit W/A/S/D break events also failed to clear retained
movement. Together with the confirmed neutral XInput packet, this rules out
both public input release paths. The failed keyboard breaks and 50 ms mouse-
yield experiment were removed to avoid prompt/culling side effects; discrete
D-pad menu navigation remains. Because the shipped executable's `.text` is
protected on disk, a one-shot runtime probe now captures Metro's decrypted
XInput consumer stack on the first analog release and writes each unique Metro
function to `vr_xinput_stack_*_rva_*.bin` beside the game. This is intended to
locate the internal movement state updated after `XInputGetState` returns.

Runtime XInput-consumer probe deployed with matching source/destination SHA-256:
`252ECF8EF002A68D5F380109E8785F5EC9AB70BCBF379D75E23AD4017ABBCC46`.

First runtime capture located the real polling routine at RVA `0x8F8A10`; its
`XInputGetState` call returns at `0x8F8A96`. The successful-input continuation
falls into adjacent MSVC unwind regions and was therefore just outside the
first 197-byte per-function dump. The probe now additionally dumps five
contiguous decrypted pages around the first Metro stack frame to
`vr_xinput_consumer_region.bin`. Expanded capture build deployed with matching
source/destination SHA-256:
`7BA2CC163FF8C16D76859106E69EFFEB98FEB6BA176A50306829BD88F494C7EE`.

Expanded capture found the exact latch in Metro's XInput consumer. At RVA
`0x8F8BE4` it reads LY/LX/RX/RY from the returned `XINPUT_STATE`. The tests at
`0x8F8C00`–`0x8F8C1F` check buttons, all four axes, and both triggers; if every
field is exactly zero, execution jumps directly to button handling at
`0x8F8DB0`. That bypasses the analog normalization and virtual callback at
`0x8F8C3E`–`0x8F8D82`, so Metro never delivers a zero vector to the object that
still holds the previous movement. Its normalizers use the standard XInput
deadzone constants: `0x1EA9` (7849) for the left stick and `0x21F1` (8689) for
the right.

Fix: a non-zero-to-zero locomotion transition arms a one-poll sentinel. The
next synthetic XInput state reports `RX=1` while LX/LY are zero. This defeats
Metro's erroneous all-zero shortcut, but `1` is far below its measured 8689
right-stick deadzone and therefore normalizes to exactly zero. Metro calls its
analog callback with four zero axes, clearing the stored movement without
turning the camera. The sentinel is included in `dwPacketNumber` state tracking
and disappears on the following poll. Runtime stack/code dumping was removed
from the normal build after identifying the consumer.

Sub-deadzone release-sentinel build deployed with matching source/destination
SHA-256: `8063E3189D2649B05E7FC6CE4AC0108F2A8DEA57BDB96919635E2DB4C0E01AC1`.

**Final headset validation: FIXED.** The player confirmed that releasing the
analog stick now stops movement. The earlier benefits remain: controller button
prompts appear much more consistently, sprint activation is substantially more
reliable, and pause-menu navigation is improved by the discrete D-pad path.
Analog speed and diagonal movement remain active during gameplay.

Final retained design:

- Gameplay movement: synthetic Xbox left stick with a radial, rescaled 0.35
  deadzone; W/A/S/D are kept released.
- Menu movement: synthetic left stick held neutral, physical movement stick
  converted to dominant-axis Xbox D-pad input.
- Release: one XInput poll with `LX=LY=0`, `RX=1`; Metro normalizes all axes to
  zero and is forced to invoke its analog callback.
- Safety: 100 ms published-axis watchdog, neutral output in adjustment/VR-menu
  modes, and the capability-safe OpenVR Axis0 touch gate remain. The touch gate
  was not the root fix, but is harmless protection for legacy profiles that
  retain their last coordinates after touch release.
- Removed after negative tests: the 50 ms culling-follow mouse-yield workaround,
  unconditional W/A/S/D break events, and runtime stack/code-dump logic.

Reverse-engineering artifacts are intentionally retained beside the game for
future reference, but the final DLL no longer creates them:
`vr_xinput_consumer_region.bin` and
`vr_xinput_stack_00_rva_008F8A10.bin` through
`vr_xinput_stack_04_rva_008E83F0.bin`. The contiguous region is the authoritative
capture of the all-zero shortcut and analog callback path described above.

#### World dragging on head movement — FIXED, and the input-device work — 2026-08-18

**World moved with the head (everything except the gun) — fixed.**
Cause was mine: the scripted pass-through gate triggered on
`|enginePitch - headPitch|`, but that quantity IS the follow error. Measured
mean follow gap 21 deg, max 108 - so the 30 deg "fast tier" (added so a creature
grab snaps the camera) cleared on ordinary head movement, the view snapped onto
the lagging engine camera, and the world dragged until the loop caught up. The
gun's body frame is yaw-only and cannot inherit a pitch lag, which is why it was
the one thing unaffected.

Confirmed numerically: mean |head - FINALview| = 15.283 deg, mean |gamePitch| =
15.285 deg. Identical - the pass-through was the entire world movement.

Fix: the gate now also requires the head to be STILL (< ~0.23 deg/frame). The
follow converges in a frame or two once the head stops, so a large divergence
with a still head is what actually implies somebody else holds the camera. A
creature grab still qualifies. Player-confirmed fixed, scripted scenes intact.

Also eliminated by measurement, do not re-examine:
- Pose staleness. Worst drift 0.06 deg over 1534 samples including fast head
  movement. A late-latch would fix nothing.
- The shake-damping filter. Reads exactly zero.
- Follow gain: raised 0.5 -> 1.0 (full correction per frame; the half-step was
  an open-loop holdover). Kept - it did not cause the drag but a follow that
  keeps up is worth having.

**Probe placement caution.** The first `pitchterms` probe sat BEFORE the
additive rebuild, so it logged the composed matrix, not the final view. It read
"view is honest" only because the two pitch sources happened to agree that run.
Any probe on the view must sit after the rebuild.

**Button prompts flipping to keyboard / first press being eaten — UNSOLVED.**
Root cause understood: every frame the follow commands is a synthetic MOUSE
event, so Metro keeps deciding the player switched to mouse. Prompts follow the
device, and the first controller press appears to be spent switching device
rather than acting. The player noticed presses behave better while WALKING -
continuous stick input wins the arbitration - which is the same effect seen from
the other side.

Three attempts, all failed:

| Attempt | Result |
|---|---|
| Memory hunt for the active-device flag (self-generated labels both sides, 96 cycles) | Best 102 hits / 89 misses of ~192 = chance. Not a plain byte in the module statics, same negative as the scripted-state hunt. |
| `kForceControllerActive` - increment the pad packet number every poll plus a 1-unit axis jitter | Did NOT hold the prompts on controller, and broke other things. Metro does not decide the device from pad-activity freshness. Reverted; do not retry via the input layer. |
| Deadzone 0.01 -> 0.05 rad (2.9 deg), to stop sending mouse events during small head sway | Helped buttons/prompts while standing still, but with full-gain correction a wide deadzone makes the camera step rather than follow, and world geometry rides on that camera - jitter while turning. Reverted to 0.01. |

Untested middle option: deadzone ~0.02 rad (1 deg), keeping some of the button
benefit with much smaller steps.

**Glyph-cell catalogue (`kGlyphCatalogue`) - built, inconclusive, left OFF.**
The remaining idea is to force the LABEL rather than the device: prompt
characters are cells in the 512x512 font atlas selected by TEXCOORD, and the
watch-digit code already reads those cells back against a verified cell map, so
rewriting a UV could turn a keyboard glyph into a controller one. The catalogue
logs each new cell and the frame it first appears, so the glyphs identify
themselves by when they show up. Its one run produced 10 cells with nothing
correlated to walking - the player was not parked at a prompt long enough.

Must stay OFF in ordinary play: six staging copies plus `Map` per frame, each a
CPU-waits-for-GPU sync, enough to be felt as jitter when turning.

Open question that decides the size of that job: if both prompts are single
atlas glyphs, it is a small UV rewrite. If Metro swaps to a different texture
for controller icons, or draws a different-length string, vertex counts change
and it is much harder.

**Cursor drift on the merchant screen - FIXED.** `SendLookInput` records the
cursor position, sends the input, and restores the position. The look arrives as
a raw-input delta; `SetCursorPos` does not generate one. Player-confirmed.

#### Current flag state — 2026-08-19 (supersedes the 2026-08-17 table above)

Read from the tree. Note the section above this one belongs with §5.16; it was
appended after §5.17/§5.18 were added by a parallel session.

| Flag | Value | Notes |
|---|---|---|
| `kAdditiveViewBuild` | `true` | yaw+yaw, pitch+pitch, roll from headset |
| `kBodyPitchFromEngine` | `false` | body contributes no pitch |
| `kLevelWeaponBodyFrame` | `true` | weapon body frame is yaw-only |
| `kPitchFollowEnabled` | `true` | **required** — proven by disabling it |
| `kScriptedPitchPassthrough` | `true` | gated on sustained divergence **and a still head** |
| `kPinCursorDuringInjection` | `true` | fixes merchant cursor drift |
| `kForceControllerActive` | `false` | tried, broke things, no effect on prompts |
| `kGlyphCatalogue` | `false` | GPU sync per glyph — off in ordinary play |
| `kInputDeviceHunt` | `false` | ran 96 cycles, chance-level result |
| `kControlStateSamplerEnabled` | `false` | F7/F8 sampler off |
| `kHuntEnabled` / `kWatchEnabled` | `false` | retired |
| follow gain | `1.0` | was 0.5 (open-loop holdover) |
| `kDeadzone` | `0.01` | 0.05 helped buttons but caused jitter; ~0.02 untested |

Log-only diagnostics still compiled in: `pitchterms`, `posestale`, `cullpitch`,
`yawresp`, `uiplace`, `VRPose script:` transitions. No behavioural effect;
remove when done.

Safety net: `d3d11.known-good-2026-08-17.dll` in the game folder.

#### Right-stick follow experiment — tried and reverted, 2026-08-19

Idea: drive the culling follow with right-stick deflection instead of synthetic
mouse, so Metro sees CONTROLLER input and the keyboard prompts / eaten first
press / menu cursor stop arising. Mouse was never a deliberate choice — Notes/13
picked it because it measured as working; the stick was never tried.

Plumbing left in place but DORMANT (`sPadFollowRX` / `sPadFollowRY`, composed
with the parallel session's neutral-dispatch pulse on RX so the two do not
fight). Both flags now `false`:

| Flag | Result |
|---|---|
| `kFollowViaStick` (pitch) | Worked on its own. Reverted only because half-stick/half-mouse is a split system with no benefit. |
| `kFollowYawViaStick` | **Failed** — jitter, view snapping back to a previous heading, shooting off. |

**Why yaw cannot be moved this way.** Pitch could be closed on the engine
because the body never pitches, so all engine pitch is ours to claim. Yaw is
not: the body has a real heading. Booking the engine's measured yaw change as
ours corrupts `sCullInjectedYaw` whenever the player is also turning, and
`gameYaw = engineYaw - sCullInjectedYaw` feeds both the rendered view (the
snapping) and `baseYaw` for the shot (the bad aim). Stick deflection has no
mouse-unit equivalent to book, so there is no correct booking available.

**The premise is dead regardless.** With BOTH axes on the stick and no synthetic
mouse being sent at all, the button prompts still flipped when the player turned
their head. So Metro does not decide the active input device from mouse events,
and removing them cannot fix the prompts. Before any further attempt, check the
other `SendInput` call sites in VRPose.cpp (there are several beyond the follow)
and establish what actually drives the device decision.

Light and medkit are safe from this: they come from the PHYSICAL VR thumbstick
and are published as D-pad buttons, so a synthetic RX/RY never re-enters that
logic.

Also confirmed while answering a question: **we already do view-matrix hooking**
(`PatchMappedVRCameraData`). The "gold standard" beyond that is writing the
player's upstream view ANGLES, which for this game has already failed once -
Notes/13's orientation scanner is retired (`kScannerEnabled = false`), and
Notes/14 shows the camera object at `0xD271B0` is an output replayed from a
command buffer, so writing it changes the picture but not culling.

### Journal objective follow-up — full native lock and clean close, 2026-08-20

User confirmed the controller-parent composition now rotates the objective text
correctly with the physical journal, but residual independent bob remained and
LT+L3 could close then immediately reopen the journal.

- Replaced the prior centre-only stabilization with a lock of the complete
  native objective-page transform after 45 continuous visible frames. The live
  `BuildWeaponAffine` parent remains outside that lock, so controller/journal
  motion still drives the text while the duplicate native basis/translation
  animation no longer does.
- When journal calibration owns input, LT+L3 now saves the surface calibration,
  exits adjustment mode, and falls through to `PublishPadButtons` in the same
  sample. This prevents the journal toggle from being delayed until after the
  objective draw vanishes and reopening the journal.
- Preserved the user's `vr_journal_surface_calibration.txt` values unchanged:
  `-0.070600 0.139144 -0.055984 0.119332 0.114026 0.000000 0.605206`.
- Release build succeeded with 0 warnings and 0 errors. Deployed game DLL SHA-256:
  `4C85C0F9D3238A46B98F5D045DEB749E1C1D5D4896FEBAB21AC11CE93A01A9A9`.

#### Correction after live test

The full native-page lock was the wrong layer. User's precise result: the
physical journal no longer bobbed, while the objective text still did. The log
also showed the native page fixed while visible text motion persisted, proving
the remaining motion is downstream of the native journal transform.

- Reverted the full native lock to the earlier centre-only stabilization, which
  keeps the journal's live native basis/animation.
- Kept the LT+L3 calibration-save/close fix.
- Kept the proven live parent orientation, but replaced its changing translation
  source with the actual controller's head-relative position plus a captured
  fixed placement offset. Pivot compensation remains live so journal rotation
  still uses the same centre.
- Build: 0 warnings, 0 errors. Deployed SHA-256:
  `E79D38841D0A4E90F6297D3BF99ADCC89B3C39FC1B08E39194DB7E7761B6CFDA`.

#### Final journal rollback for now

The controller-derived parent-translation experiment may have removed some bob
but made the overall journal/text behavior worse. Removed that experiment and
returned rendering to the last stable centre-lock + live native basis + exact
weapon-parent composition. The independent minor text bob is accepted for now.
Retained the separate LT+L3 save/exit/close fix and the user's saved calibration.

Build succeeded with 0 warnings and 0 errors. Deployed DLL SHA-256:
`B67695383914E03A567E77193FB9B9D12747FAF3D8C3CB1F90A5D9D3B19FF94E`.

#### Exact requested rollback clarification

User clarified that the desired state is the build immediately before the
journal-bob removal experiment. The first rollback incorrectly retained the
later LT+L3 calibration-close change. Restored the complete earlier functional
state: centre-only stabilization, live native journal basis, exact weapon parent,
and the original calibration input ownership/close behavior. Saved surface
calibration remains unchanged. Rebuilt and deployed with 0 warnings/errors.

Deployed DLL SHA-256:
`F652EBCAE7D7AA6AC9AA772F45D6AB917E31B9DF3CB974BFC84FE976A98E215E`.

#### Rollback boundary corrected again — fully live native page

Live test showed the physical journal still lacked its bob. The requested good
state actually predates the centre-only stabilization, not merely the later full
page lock. Removed every native-page/centre settling lock. `nativePage` and
`pageCentre` are now live directly from Metro's objective transform on every
frame, restoring the journal-linked animation. The established calibration and
weapon-parent composition remain; original calibration input handling remains.

Build succeeded with 0 warnings and 0 errors. Deployed DLL SHA-256:
`AF900FC996B036A9F9F787553F146C770A37E4A57CB134947EEE75CC7140A2C8`.

### Journal objective — accepted final state, 2026-08-20 20:19 CDT

This section is authoritative over the preceding journal experiments.

#### Result kept

- The missing objective text is visible in VR, fused into one stereo image, and
  attached to the right-controller/journal transform.
- It follows the journal's rotation correctly.
- The user can position, rotate, and scale it with the journal calibration mode.
- Metro's fully live native journal page transform is used; all experimental
  centre/full-page settling locks and the controller-derived translation
  replacement were removed.
- Some independent text bob/movement remains. Further attempts to suppress it
  either removed the physical journal's bob or made overall tracking worse, so
  this limitation is accepted for now.
- Original calibration input ownership/close behavior is present in this build;
  the later LT+L3 close experiment is not included.

#### Active saved calibration

File: game-directory `vr_journal_surface_calibration.txt`

```text
-0.073615 0.141142 -0.058959 0.119332 0.114026 0.000000 0.605206
```

Meaning: position `(right, up, forward)`, rotation `(pitch, yaw, roll)` in
radians, then uniform scale. The file timestamp was `2026-08-20 20:19:17 CDT`,
and `d3d11_log.txt` confirmed the same values were explicitly saved when journal
adjustment mode was turned off.

#### Deployed build

- Release build: 0 warnings, 0 errors.
- Game `d3d11.dll` SHA-256:
  `AF900FC996B036A9F9F787553F146C770A37E4A57CB134947EEE75CC7140A2C8`.

### Journal calibration removal and performance-diagnostic cleanup, 2026-08-21

The final journal calibration was baked directly into
`BuildJournalScreenMatrix`:

```text
position = (-0.073615, 0.141142, -0.058959)
rotation = ( 0.119332, 0.114026,  0.000000) radians
scale    =  0.605206
```

Removed the journal calibration mode, its controller-input ownership, load/save
code, render-to-input notification, and `VRPose.h` API. Deleted the now-unused
game-directory `vr_journal_surface_calibration.txt`; its exact values remain
baked above and documented here.

#### Performance audit and cleanup

The last live log was 35.8 MB and confirmed several retired diagnostics were
still active:

- `VRPose posestale`: 22,241 entries and, more importantly, an extra OpenVR
  `GetDeviceToAbsoluteTrackingPose` query from the render path for every sample.
- Controller raw/derived ratio tracing: 5,946 entries at one sample per six
  calls.
- ADS instance, weapon calibration, UI placement/hit, journal per-frame, laser
  sequence, and rendered-vs-culling-angle diagnostics.
- One-shot camera/matrix hardware-breakpoint stages, weapon mesh GPU readbacks,
  automatic stereo/instance censuses, and the every-draw muzzle-flash hunt.

All of those diagnostic paths are now disabled. In particular, the mesh
measurement staging-buffer Maps and full-frame censuses can no longer introduce
one-shot GPU stalls, and the flash hunt no longer maintains a set on every draw.
Functional systems that happen to retain historical "probe" names—especially
the fire hook/fallback used for controller-directed shots—were deliberately
kept enabled.

Release build succeeded with 0 warnings and 0 errors. Deployed DLL SHA-256:
`36DDBDADFDEA928FA944C229F9EDD3A807B90B4792A40164677311B0E526B985`.

### Equipment and weapon menus — 2026-08-20 21:11 CDT

#### Equipment-menu A/B input

- While LT's equipment modifier is held, the physical A and B face-button
  edges are now forwarded to Metro alongside the existing equipment inputs.
- Live test confirmed both menu choices work.
- Confirmed deployed build SHA-256 began `B277` (superseded below).

#### Weapon-menu preview meshes

- A bounded LT-menu draw census identified the three gun previews as eleven
  indexed mesh draws using VS `84870CD85AE567EF` and PS
  `9F4E9CF785DEFD17`, rendered by Metro's old `HUD Part 3` path rather than the
  ordinary 2D sprite UI.
- Removed the broad census and added an exact-shader private `cb1` projection.
  It preserves Metro's native three-slot weapon layout, applies the same 70%
  compact scale and horizontal centring as the VR menu rectangles, and builds
  distinct left/right convergence at the shared one-metre UI plane.
- The preview family is kept off delayed second-eye batching so each draw uses
  the private right-eye projection rather than stale shared bytes.
- Release build succeeded with 0 warnings and 0 errors. Deployed game DLL
  SHA-256:
  `686D7C150F32CE249A3F202DA92EACC25944694D92BA9FF4F48B9AEEBE340FC9`.
- Awaiting headset confirmation of final placement and convergence.

#### Screenshot correction — keep fusion, restore native layout

- Headset test confirmed the private per-eye preview path fused the gun icons.
- The added desktop-projection/70% remap was independently wrong: it moved the
  meshes above the three boxes and collapsed their horizontal spacing.
- Removed that layout remap. The exact preview draws still use private eye-0
  and eye-1 buffers and remain excluded from delayed batching, but those
  buffers now preserve the already-correct native placement bytes unchanged.
- Release build succeeded with 0 errors (8 pre-existing compiler warnings).
  Deployed game DLL SHA-256:
  `B8E22972C5A69B43FD9BAEE0D6864641EB4B65B1B56C6AEA0322C9616CE4C343`.
- Awaiting confirmation that fusion remains while the icons return to the
  bottom rectangles.

#### Combined native placement + proven stereo delta

- Test of the native-byte build returned fully to the original state: correct
  bottom placement but doubled icons. This proves the earlier full projection
  replacement—not merely the private buffer—was responsible for fusion.
- New combination keeps eye 0's native preview projection unchanged. Eye 1 is
  derived from that same matrix with only the x/y NDC delta measured from the
  already-working finite-depth VR UI plane. Scale, depth, and the three native
  slot placements therefore remain untouched while the eyes receive the
  convergence difference that fused the previous build.
- Release build succeeded with 0 errors (8 pre-existing warnings). Deployed
  game DLL SHA-256:
  `3AC2E6006ADEAC77D8214E87042F58D58D93EDD013FABB0116AFAC5EE5FF137F`.

#### Accepted weapon-menu result

Live headset test confirmed the weapon icons are fused and remain at the
bottom in their native three-slot placement. Keep the combined native-eye-0
layout plus eye-1 UI-plane disparity approach. The equipment-menu A/B input
fix also remains active in this build.

#### Placement clarification — target is the radial-height box row

The preceding “accepted” wording was incorrect. The user confirmed fusion but
clarified the native bottom row is still the wrong location. The intended three
rectangles are the horizontal row immediately to the right of the radial wheel,
as shown in the supplied non-VR reference.

- Measured native per-eye preview centres at approximately `(278,420)`,
  `(368,420)`, `(454,420)` and target box centres at approximately `(353,353)`,
  `(415,353)`, `(477,353)` in the supplied side-by-side capture.
- Applied a shared clip-space affine to the native projection: 0.70 x/y scale,
  `+0.277` NDC right, `+0.098` NDC up. This preserves the mesh family and
  relative three-slot layout instead of substituting the desktop projection.
- The confirmed eye-1 UI-plane disparity is applied afterward, so placement
  changes identically for both eyes and should retain fusion.
- Release build succeeded with 0 errors (8 pre-existing warnings). Deployed
  game DLL SHA-256:
  `42156247709C71F529BE421EE09FB7BA1842028EAA4E00E2E16BEDA29500D847`.
- Awaiting live confirmation; do not mark the weapon menu accepted yet.

#### Move the dark slot panels behind the corrected previews

- Live screenshot confirmed the gun previews are fused and in the intended
  radial-height row, while the three dark panel layers remain in their native
  bottom positions.
- The panel row has the same measured source-to-target mapping as the previews.
  During the physical weapon-menu hold only, generic panel shaders
  `C03E9887599690ED`, `42E7BD30BAF692E2`, and `9DB1461C75F6D601`
  now receive the identical 0.70 scale / `+0.277` NDC right / `+0.098` NDC up
  affine in both eyes. Other uses of these shared UI shaders are untouched.
- Native draw order is preserved, so the relocated panels should remain behind
  the gun meshes.
- Release build succeeded with 0 errors (8 pre-existing warnings). Deployed
  game DLL SHA-256:
  `F427C0B264777AAAC123A1C2E1BF4408B1410D2AC83E7D13B248E651353EA3EA`.

#### Panel-shader guess rejected; exact box census armed

- Live screenshot showed the three dark boxes did not move, while unrelated
  small chrome/icons did. Reverted the generic panel-shader affine completely;
  the corrected, fused gun previews remain unchanged.
- Added a one-complete-frame, read-only census armed on the physical weapon-menu
  hold. It records draw order, VS/PS hashes, call/counts, PS texture pointer and
  dimensions, input layout/VB/stride/offset, and scissor rectangle. This is to
  identify the actual box draws without another broad visual match.
- Release build succeeded with 0 warnings and 0 errors. Deployed diagnostic DLL
  SHA-256:
  `2AC749EC30B1879B2B09376D3374122F4E45505C967DF1B5E979939BD850B777`.
- Next test: launch, hold the weapon menu open for at least two frames, release,
  and close Metro so `d3d11_log.txt` can be inspected.

#### First census timing corrected — quad-coordinate probe

- The first census captured frame 1537 before the exact preview shader appeared,
  so it described the ordinary gameplay/HUD tail rather than the active weapon
  wheel. It could not identify the boxes.
- Replaced it with a bounded probe that arms only after VS
  `84870CD85AE567EF` / PS `9F4E9CF785DEFD17` is actually observed. It then
  reads back at most 48 following six-vertex `F5F5C7E1E2F0831E` sprite quads
  and logs all seven float-component bounds plus PS, texture size, frame, and
  first vertex. This deliberately incurs short diagnostic GPU stalls and must
  be removed after the box geometry is identified.
- Release build succeeded with 0 warnings and 0 errors. Deployed diagnostic DLL
  SHA-256:
  `FD689FD75DEF885DE830B5CDBEC0886E92E44F8C07BC4B7E0ED1F283EE156D69`.

#### Exact weapon-slot backgrounds relocated

- The coordinate probe identified the four actual dark background quads. They
  use VS `F5F5C7E1E2F0831E`, PS `C03E9887599690ED`, a 1024x1024 atlas, six
  vertices, and first-vertex offsets `1316`, `1328`, `1340`, and `1359`.
- Removed the diagnostic GPU readback completely. During the physical weapon
  menu hold, only those four exact draws now receive the same 0.70 scale /
  `+0.277` NDC right / `+0.098` NDC up affine as the already-correct gun
  previews, independently for both eyes. The unrelated chrome that moved in
  the rejected broad-shader attempt is no longer matched.
- Release build succeeded with 0 errors (8 pre-existing warnings). Deployed
  game DLL SHA-256:
  `A19A6BAD51D8CCC2CE1F698BDDC280F40C335253D72F37CBCB90300AF2C0B439`.
- Awaiting live confirmation that the dark boxes now sit behind the guns and
  the surrounding weapon-menu elements remain in their native positions.

#### Weapon-slot signature corrected for dynamic vertex packing

- Live test showed the boxes still at the bottom. The four `FirstVertex`
  offsets from the coordinate probe were positions in Metro's dynamically
  packed UI vertex buffer, not stable draw identities, so the exact-offset gate
  could fail on the next launch.
- Removed only that unstable condition. The box transform remains constrained
  to the physical weapon-menu hold, VS `F5F5C7E1E2F0831E`, PS
  `C03E9887599690ED`, a six-vertex non-indexed draw, and the 1024x1024 atlas.
  The previously problematic PS1/PS2 chrome families remain excluded.
- Release build succeeded with 0 warnings and 0 errors. Deployed game DLL
  SHA-256:
  `0B55C3373679CC2DD32ED25FB56895B3E0DCE1EB7B6903488A059894CA47F5E6`.
- Awaiting live confirmation.

#### Reconstructed the confirmed `42156247...` weapon-menu baseline

- The latest screenshot showed that the C03 experiment moved bright blurred
  rectangles above the gun row while the black boxes remained at the bottom.
  This proves those C03 draws are not the requested black boxes.
- Removed the entire post-baseline box experiment: its draw flag, C03
  classifier, 2D screen-CB affine, eye-1 affine, and associated state. The
  private 3D gun-preview correction is unchanged, as is the working equipment
  menu input fix.
- No archived DLL with exact SHA-256
  `42156247709C71F529BE421EE09FB7BA1842028EAA4E00E2E16BEDA29500D847`
  was present, so its source behavior was reconstructed by reverting only the
  later panel work. Release build succeeded with 0 errors (8 pre-existing
  warnings). Deployed game DLL SHA-256:
  `BF371112A38CC35ED8E18DB5D39633D08CCC8C57A611E6EBB2A46F9C51AC4828`.
- Next test should confirm the gun row and the bright rectangles are back to
  the `42156247...` layout before any new black-box investigation.

### 5.14 Physical weapon calibration restored; menu offsets disabled — 2026-08-20

- LT + left grip calibration appeared unable to move the held weapon. The VR
  menu was not consuming that chord while closed; the actual blocker was the
  runtime weapon matcher reapplying the saved profile at every frame boundary
  while physical calibration was active, immediately undoing stick changes.
- `ObserveWeaponCalibrationMesh` now skips saved-profile selection for the
  entire time `sWeaponAdjustMode` is active. Entering physical calibration also
  clears any stale unsaved VR-menu preview lock, and a successful physical save
  clears it again.
- The VR menu Offsets tab is temporarily non-interactive as requested. It now
  displays `TEMPORARILY DISABLED` and `USE LT + LEFT GRIP`; its sliders and
  `SAVE OFFSETS` action cannot run.
- Release x64 build succeeded with 0 warnings and 0 errors. Source and deployed
  game DLL SHA-256:
  `149CD89B7F20EF6A6C05C41E2560021620A6BFD4299CC2DACBCD022E94E6CF26`.
- Awaiting live confirmation that LT + left grip can enter calibration, that
  left/right sticks move and rotate the weapon without snapping back, and that
  a second LT + left grip hold saves the resulting profile.

### Resolution menu restart-only experiment — rolled back 2026-08-21

- A startup-captured compositor scale was added so Picture-tab changes would
  remain pending until restart. The first test build then terminated during
  the intro-to-gameplay transition at frame 715. The saved and active scales
  were both exactly `1.0000`, so the scaler itself took the same no-op branch
  as the preceding build, and the runtime log contained no direct fault.
- With no WER dump or Application Error event, the change was rolled back in
  isolation for an A/B launch. Resolution changes are temporarily live again.
  If the transition succeeds with this build, redesign the restart boundary;
  if it still fails, the crash predates and is independent of the latch.
- The rollback rebuild still crashed. Cross-task history then confirmed that
  rebuilding the current dirty source had already caused the same regression
  before the resolution work; restoring `d3d11.dll.bak` had fixed it. The exact
  stable backup was restored again and SHA-256 verified as
  `686D7C150F32CE249A3F202DA92EACC25944694D92BA9FF4F48B9AEEBE340FC9`.
  Do not rebuild/deploy the current dirty tree as a presumed stable baseline.
  The resolution restart behavior must be reconstructed on top of the source
  state corresponding to this stable binary, or after the later UI/render
  regression is isolated.

### Known-good `686D7C15...` source reconstruction — 2026-08-21

The exact source tree used to build the stable DLL was not committed at build
time, so it cannot be recovered as a bit-identical Git checkout. It was
reconstructed from the surviving known-good DLL, the recorded per-change diffs
in the Journal Fix and Equipment and Weapon Menus tasks, and the build hashes
and behavior recorded above.

- Restored the original runtime journal calibration mode, load/save path,
  render notification, input ownership, and accepted saved calibration values.
- Restored the exact first compact stereo weapon-preview projection algorithm
  recorded for `686D7C150F32CE249A3F202DA92EACC25944694D92BA9FF4F48B9AEEBE340FC9`.
- Re-enabled the diagnostic compile-time states that were present at that build
  boundary; their later cleanup was bundled into the crashing `36DDBDAD...`
  build and was therefore not retained in this reconstruction.
- Release x64 solution build completed successfully. Candidate SHA-256:
  `E474FE7BFD43969989C8E1B0F615BE5CE424A00E00AADA042711DA3F9EC289A9`.
- The original stable DLL was preserved as
  `d3d11.known-good-686d-before-reconstruction-test.dll` before deployment.

This is a source-level behavioral reconstruction, not a claim of binary
identity.

Post-reconstruction launch check: the user confirmed that the active original
`686D7C15...` DLL passes the pre-rendered intro, enters gameplay, and looks
correct. This reconfirms the recovered binary as the known-good reference. The
separately built `E474FE7B...` reconstruction candidate was not deployed during
that check and therefore still requires its own A/B headset validation.

#### Reconstruction candidate validated in headset

The `E474FE7B...` candidate and its matching PDB were subsequently deployed.
The active game DLL hash was verified before and after the test. The user
confirmed that this reconstructed build passes the pre-rendered intro, enters
gameplay, and looks correct in the headset. The reconstruction branch is now
the confirmed reproducible stable source baseline for subsequent resolution
restart work. The original `686D7C15...` DLL remains available as a rollback.

### Resolution scale latched until restart — candidate 2026-08-21

- `VRMenu::Initialize` now captures the saved resolution scale exactly once as
  the applied startup scale. The renderer and any render-resolution scaling
  path use only this immutable value for the lifetime of the process.
- Picture-tab adjustments still update and save `Settings::resolutionScale`,
  but that value is explicitly the next-launch target and cannot recreate the
  compositor submission textures while the menu is open.
- The Picture tab now reports the active render target from the startup scale
  separately from the saved target. It displays `RESTART REQUIRED` only while
  those values differ, and `ACTIVE` once the launched scale matches the file.
- Release x64 solution build succeeded. Deployed candidate SHA-256:
  `F90EE27573E28FDC8B907213182508D6EFE929986E4C8BAA1250BEB95FC1FA0B`.
- The verified reconstruction build `E474FE7B...` and original `686D7C15...`
  build are both preserved as rollback DLLs. Awaiting a two-launch test: change
  scale without an immediate active-resolution change, then restart and verify
  the saved target becomes active without an intro-to-gameplay crash.

Headset result: confirmed working. Changing the Picture-tab resolution leaves
the active target and menu rendering intact, and the saved target takes effect
after relaunch. The game also passes the intro-to-gameplay transition. Commit
`da19206` is the verified restart-only resolution checkpoint.

### Native supersampling above 1x — candidate 2026-08-21

- Split the applied resolution setting into two non-overlapping paths. Values
  from `0.5x` through `1.0x` retain the proven compositor post-render
  downsampling path. Values above `1.0x` cap compositor scaling at `1.0x` and
  instead save Metro's native `r_supersample` cvar for the next launch.
- Native `r_supersample` is clamped to at least `1.0`, so LOW/MED cannot combine
  engine undersampling with compositor downsampling. ULTRA and manual values
  above `1.0` render additional scene pixels before Metro's native resolve,
  rather than enlarging an already-rendered eye image.
- Picture-tab scale changes update `user.cfg` immediately and schedule a hidden
  post-exit rewrite sourced from `vr_menu_settings.txt`, because Metro writes
  its own cvars during shutdown. The helper also applies the `1.0` lower clamp.
- Release x64 solution build succeeded. Deployed candidate SHA-256:
  `40E79F6F362756EEBCD4CFC21C0EFCFE22F3FC38566B9ADF360F37C2EE18E8B2`.
- Verified restart-only build `F90EE275...` is preserved as a rollback. Awaiting
  a two-launch ULTRA/manual-above-1x test and confirmation from the logged scene
  target dimensions that the increase is native rather than compositor-only.

Headset result: `r_supersample 1.5000` left the scene targets fixed at
`3620x2009`; there was no visible-quality or frame-rate change. The native-cvar
sync and post-exit helper were removed as ineffective.

### OpenVR baseline and oversized-output recovery — verified 2026-08-21

- OpenVR reports a recommended render target of `3160x3360` per eye. Metro
  normally submits two independent `2560x1440` eye textures while its main
  scene inputs remain `3620x2009`.
- The previously suspected renderer globals at `metro.exe+0xD01E64/+0xD01E60`
  are still `0x0` when D3D is created. A version-guarded experiment hooking the
  apparent native resolution loader installed successfully but was never
  invoked and produced no root-resolution change; it was removed.
- During that test Metro selected the NVIDIA DSR/DLDSR fullscreen mode
  `5760x3240` even though `user.cfg` remained `2560x1421` and the physical
  display was `2560x1440`. Metro continued using `1421`-high presentation
  viewports, producing a valid upper image with stretched/undefined rows below.
- Added a narrow swapchain guard that clamps output requests larger than the
  known `2560x1440` presentation surface. It runs during legacy/factory2 swapchain
  creation and later `ResizeBuffers`/`ResizeTarget` calls; internal scene targets
  are not modified.
- Runtime verification logged the `5760x3240 -> 2560x1440` clamp at initial
  creation and subsequent Video-menu rebuilds. Submission textures returned to
  `2560x1440`, and the user confirmed the headset image is back to normal.
- Verified active DLL SHA-256:
  `38882D8270EEA589D569F8A9BDE649BA2188A86196AB4FB885956B6AB5FAB218`.

### Renderer-wide scene scaling above 1x — verified pipeline stage 2026-08-21

- Allocation-stack tracing identified Metro's actual scene dimensions at
  `metro.exe+0xD044B0/+0xD044B4`. The renderer-wide setter at `+0x839540`
  updates these globals together with dependent viewport and aspect state.
- Scaling only later setter calls produced `4525x2511` viewports over existing
  `3620x2009` textures and visibly corrupted the image. An attempted bootstrap
  at D3D-device construction also ran too early, while the globals were `0x0`.
- The working synchronization point is Metro's first `3620x2009` texture
  allocation. The mod invokes Metro's own setter there, resizes that in-flight
  descriptor, and keeps the two confirmed later scene-setter callers scaled.
- At `1.25x`, runtime verification showed `4525x2511` viewports, render targets,
  and final-pass inputs. At `2.0x`, all three reached `7240x4018`; the image
  remained correctly framed in the headset.
- Active candidate SHA-256:
  `EC707E96776E706A2E0E3C1F6683FC705795A8901286CD1B24D431E259E35651`.
- This proves native scene scaling, but Metro still resolves to and submits a
  `2560x1440` backbuffer, so the visual benefit is heavily bottlenecked. The
  next stage is a synchronized high-resolution base canvas/swapchain buffer
  while retaining the physical-output DSR guard.

### High-resolution VR output and menu compatibility — verified 2026-08-22

- Added an isolated high-resolution final-colour path for resolution scales
  above `1.0x`. Metro's final sRGB scene array is captured at the exact final
  shader pair and copied into independent per-eye UNORM output textures. This
  preserves Metro's intended colour conversion while retaining the additional
  scene detail; the user verified both the sharper image and correct brightness
  at `2.0x`.
- Registered the high-resolution outputs as stereo twins and redirected the
  native-resolution overlay stages into them. Loading screens, the pause menu,
  and the equipment/weapon UI now remain correctly framed without visibly
  switching the entire headset image back to the lower resolution.
- Restored the proven native weapon-menu layout and eye-1 UI-plane disparity.
  Weapon preview draws are forced through the immediate stereo-twin path so the
  gun icons render at the intended size and fuse between the eyes at `2.0x`.
- Removed the broad first-900-frame mono presentation override. It had made
  both eyes display the left image during startup before the right eye snapped
  into stereo. The user verified that startup now remains fused and transitions
  normally.
- Release x64 solution build succeeded. Verified active DLL SHA-256:
  `68B5D438654E06100CD2299540C55AE35A5E4F4D40E35170E4445395AE2AAA1F`.
- Headset result: verified at `1.0x` and `2.0x`; gameplay colour and sharpness,
  loading/pause/equipment menus, weapon icons, and startup stereo are working.

### Live VR brightness control — verified 2026-08-22

- Connected the existing Picture-tab brightness setting to a final per-eye
  submission pass, making changes visible immediately in the headset while the
  VR Menu is open. `0.50` is exactly neutral; the range applies gamma `2.0`
  through `0.5`, darkening or revealing shadow detail without lifting black.
- The adjustment is applied after the complete eye image is assembled, so it
  covers gameplay and menus and works with both the native and high-resolution
  submission paths.
- The first candidate inherited Metro's render state. Menu alpha blending then
  accumulated prior frames, causing movement smearing and corrupt presentation
  during the post-video intro, main menu, loading screen, and pause menu.
- The final pass now uses explicit no-blend, no-depth, unclipped raster state
  and restores Metro's render targets, viewports, input assembly, shaders,
  resources, constant buffers, rasterizer, depth/stencil, and blend state.
- Release x64 solution build succeeded. Verified active DLL SHA-256:
  `78EC23954FD02083534362413628BA01A180FE1D9D3DE32C1E29DDE4880EA65D`.
- Headset result: live brightness works, persists across restart, and no longer
  disrupts the post-video intro, main menu, loading screen, or pause menu.

### Dynamic Metro SSAA compatibility — all modes verified 2026-08-22

- Replaced the fixed `3620x2009` X2 assumption with runtime detection of
  Metro's active SSAA scene canvas. Verified canvases are `3620x2009` for X2
  and `2560x1421` for Off; the high-resolution render graph now derives its
  scaled targets and final-pass capture from the detected dimensions.
- Added a safe live-change guard. If Metro rebuilds its SSAA canvas while the
  game is running, high-resolution assumptions are disabled until restart so
  the Video menu remains readable and the new canvas can be detected cleanly
  at the next launch.
- Normalized native overlay coordinates against the detected canvas ratio.
  The merchant screen now matches X2 under Off, while the Off-only pause fix is
  gated away from X2 to prevent intermittent undersized pause notebooks.
- Corrected the held equipment and weapon interfaces independently. Equipment
  retains its native Off placement; the weapon sprite layer uses the detected
  X2/Off ratio plus its measured anchor, while its separate 3D gun previews use
  their own vertical correction and per-eye stereo projection.
- Removed temporary pause-draw and full weapon-matrix diagnostics after the
  fixes were verified in the headset.
- Release x64 solution build succeeded. Clean checkpoint DLL SHA-256:
  `49503105D1831DFA55A52AF455787E05D884ECB30BF8901B179650F247534C42`.
- Headset result: X2 remains correct; SSAA Off gameplay, pause, equipment,
  weapon selection, merchant UI, loading, and startup presentation are all
  verified working.
- Detected Metro's 0.5x scene canvas as `1810x1004`. At this tier Metro authors
  UI vertices directly against the reduced canvas, so the shared UI plane now
  accepts a sub-1.0 authored-coordinate ratio instead of incorrectly clamping
  it and shrinking every menu.
- Added a 0.5x-only equipment-wheel centering correction after comparing the
  headset captures against X2. Pause, equipment, weapon selection, and merchant
  interfaces are verified at 0.5x without changing the verified Off/X2 paths.
- Release x64 solution build succeeded. Verified 0.5x DLL SHA-256:
  `AA72A87C1813FA2C590BCE6F256B4C89E0CB80912FCC2AE97230C17E6FF0AFD3`.
- Detected Metro's 3x scene canvas as `4434x2461`. Its pause overlay retains
  X2-authored coordinates, while equipment, weapon, and merchant interfaces
  also carry the active VR resolution scale. Those paths are normalized
  separately, leaving the independent 3D weapon previews unchanged.
- Release x64 solution build succeeded. Verified 3x DLL SHA-256:
  `7777DF263D793F48E4201BBD357FDCAC72BDA9D9249B0EBBD573DC3C35FDDA81`.
- Headset result: 3x gameplay, pause, equipment, weapon selection, and merchant
  UI match the verified X2 presentation.
- Detected Metro's 4x scene canvas as `5120x2842`; with the active VR 2x
  resolution scale, its isolated output is `10240x5684`. The verified 3x
  normalization generalizes to 4x using the detected canvas ratio.
- Release x64 solution build succeeded. Verified 4x DLL SHA-256:
  `8E0018CED65CE407CB4104D22750A698A26FBE5FCA586A2A2BC573C0D6BC215F`.
- Headset result: 4x gameplay, pause, equipment, weapon selection, and merchant
  UI match the verified X2 presentation. Metro SSAA Off, 0.5x, X2, 3x, and 4x
  are now all verified.

### Final resolution, brightness, and SSAA handoff — 2026-08-22

This is the consolidated reference for the resolution-related work completed
and headset-tested during this development pass.

#### VR resolution behavior

- VR Menu resolution changes are restart-only. Moving the slider records the
  requested scale without rebuilding Metro's render graph underneath an active
  menu; the selected value takes effect on the next game launch.
- Scales below `1.0x` retain the proven compositor-side reduction path. Scales
  above `1.0x` resize Metro's detected scene targets and therefore produce real
  additional scene detail rather than asking OpenVR to resample an unchanged
  source image.
- The high-resolution path captures Metro's final sRGB scene-array output into
  independent per-eye UNORM textures. This fixed the initially brighter image
  while preserving the sharpness that was visible at VR resolution `2.0x`.
- Loading screens, pause screens, held equipment/weapon interfaces, and other
  late overlays are redirected into the high-resolution eye outputs. The mod
  no longer flips the whole headset image down to native resolution to display
  those interfaces.
- The final active test configuration used VR resolution `2.0x`. Consequently,
  Metro SSAA 4x (`5120x2842`) produced isolated eye targets of `10240x5684`;
  this is valid but exceptionally expensive.

#### Metro SSAA canvas matrix

| Metro setting | Detected scene canvas | UI handling | Headset result |
| --- | ---: | --- | --- |
| Off | `2560x1421` | Native-canvas overlay and inventory corrections | Verified |
| 0.5x | `1810x1004` | Sub-1.0 authored-coordinate normalization plus equipment centering | Verified |
| X2 | `3620x2009` | Reference canvas used to tune the VR UI plane | Verified |
| 3x | `4434x2461` | High-SSAA overlay/inventory normalization | Verified |
| 4x | `5120x2842` | Same detected-ratio high-SSAA normalization | Verified |

#### Menu and presentation details

- Metro uses different coordinate conventions for late pause/merchant overlays,
  held inventory sprites, and the separate 3D weapon-preview draws. Treating
  them as one layer caused the repeated scale, placement, and gun-icon errors.
- The equipment wheel and weapon wheel now have independent placement rules.
  The weapon sprite panel is normalized separately from its three 3D gun
  previews, which retain their native slot layout and receive per-eye stereo.
- SSAA Off has its own verified merchant, pause, equipment, weapon-panel, and
  weapon-preview corrections. The 0.5x tier additionally receives a measured
  equipment-only horizontal correction.
- At 3x and 4x, pause/merchant overlays retain X2-authored coordinates while
  the held inventory sprites also carry Metro's enlarged canvas and the active
  VR resolution scale. The final transform preserves those factors separately.
- The startup first-900-frame mono workaround was removed. It had caused an
  initial eye mismatch before the right eye snapped into place; startup now
  remains fused through the post-video scene.
- Live brightness is applied only in the final per-eye submission pass, using
  explicit no-blend/no-depth state and complete state restoration. It no longer
  smears or corrupts the intro, main menu, loading screen, or pause menu.

#### Runtime and recovery rules

- The fixed X2 canvas assumption was replaced with runtime scene-canvas
  detection. Target sizes, eye textures, overlay routing, and final capture now
  derive from the detected Metro SSAA canvas.
- If Metro's SSAA/video setting changes while running, the mod disables stale
  high-resolution assumptions and uses a readable native fallback until the
  next launch. A full restart is required before evaluating the newly selected
  SSAA mode.
- Oversized DSR/DLDSR desktop modes remain guarded so they are not mistaken for
  Metro's scene canvas.

#### Verified Git checkpoints and active binary

- `da19206` — apply VR resolution changes on next launch.
- `345462d` — confirm restart-only resolution behavior.
- `f44c28c` — scale Metro scene targets above `1.0x`.
- `b80deed` — finalize high-resolution colour, overlay, weapon-menu, and startup
  presentation handling.
- `01b17ed` — add verified live VR brightness control.
- `72cb3f9` — detect and support dynamic Metro SSAA canvases; verify Off/X2.
- `7963764` — normalize and verify SSAA 0.5x menus.
- `9a48eab` — normalize and verify SSAA 3x menus.
- `7f47bd5` — normalize and verify SSAA 4x menus; all Metro modes complete.
- SSAA work branch: `codex/ssaa-compatibility`.
- Final verified deployed DLL SHA-256:
  `8E0018CED65CE407CB4104D22750A698A26FBE5FCA586A2A2BC573C0D6BC215F`.

### 2026-08-23 — left-watch exact-parent baseline (awaiting headset test)

- The isolated left-hand range now publishes the exact completed 64-byte rigid
  instance that is submitted unchanged to both eyes.
- The identified 6108-index physical watch casing and separate 1296-index
  display surface consume that completed instance directly. They no longer
  reconstruct a parallel left-controller transform, following the same
  parent-affine principle that fixed objective text rotation on the journal.
- This baseline deliberately bypasses (but does not modify or delete) the old
  `vr_watch_offset.txt` values. The current left-hand calibration is also
  untouched. This separates parent/rotation correctness from later local watch
  placement work.
- Watch routing is gameplay-gated and accepts the immediately previous frame's
  hand instance if Metro submits the watch before the hands. Periodic logging
  reports the watch draw's index count and parent-frame age.
- Branch: `codex/left-hand-controller-attachment`.
- Candidate: `Builds/left-watch-exact-hand-parent-20260823/d3d11.dll`.
- Deployed DLL SHA-256:
  `6B254B8B3B5E2184460D2C826ED9670DD96D2BE84A01E58141CE2E0F29B41BEE`.

**Baseline result and discriminator correction:** headset testing showed a gun
component attached to the left controller, not the watch. The runtime log made
the cause conclusive: only the broad `IndexCount=1296` route fired. This agrees
with the earlier affected-scene notes, which said the supposed 1296 watch layer
never fired there and that the candidate was removed. The actual 6108 watch
route did not fire because the baseline had also added a scene-specific shader
pair requirement.

The corrected build removes 1296 from left-hand routing completely and restores
the earlier headset-proven physical-watch discriminator: the 6108-index
single-instance viewmodel draw. Unlike the old attachment experiment, that
proven draw now consumes the exact completed left-hand instance, so this test
retains the new parent-rotation architecture without guessing a new mesh ID.

- Corrected candidate:
  `Builds/left-watch-6108-exact-hand-parent-20260823/d3d11.dll`.
- Corrected deployed DLL SHA-256:
  `43F267F6B1E52B1191C5CEABD8E23850200A170FADB59AD8F63074276B3EC7C2`.

**6108-only result and native-child reparent fix:** video
`2026-08-23 10-08-13.mp4` showed the physical strap/casing moved left but sat
away from the wrist and orbited during controller rotation, while the illuminated
watch surface remained beside the right-hand gun. The runtime log confirmed the
6108 route was current-frame (`frameAge=0`), ruling out stale-pose lag.

The saved RenderDoc capture explains the orbit: the native hands instance is
nearly identity, but the 6108 watch has its own large authored rotation and
translation. Writing the completed hand instance directly over the watch erased
that child transform. The physical watch now uses the exact hierarchy:

`finalWatch = completedLeftHand * inverse(nativeHands) * nativeWatch`

The remaining illuminated surface is the exact watch-icon anchor followed by
five generic-text draws (`HH:MM`). Those draws now follow the same parent delta
using the journal's accepted reconstruction principle: remove Metro's game
projection from each source matrix, apply the left-hand parent delta to the
native watch surface, then reapply the eye projection and view correction. This
preserves Metro's authored size, spacing, orientation, and watch animation rather
than rebuilding a guessed wrist plane.

- Candidate: `Builds/left-watch-native-child-reparent-20260823/d3d11.dll`.
- Deployed DLL SHA-256:
  `8918D3F8A7A32388C6B0DA5C4507EB87FA8C06888159968CE325B1793CBBF5E2`.

**First attached-time result and complete physical-watch identification:** video
`2026-08-23 10-19-16.mp4` confirmed the live `HH:MM` string was successfully
controller-attached for the first time. It also showed the blue/orange hardware
still on the right controller and the time separated from the casing.

Per-draw image differencing of `watch_live_frame5912.rdc` identified the missing
physical parts without relying on index counts alone. The 6108 casing, the
2628-index draw, and both 750-index material passes carry the exact same native
instance matrix, including all 12 affine values. The latter draws are now routed
only when their live native matrix matches the current frame's 6108 matrix within
0.001; an unrelated 750/2628 mesh therefore remains untouched. The disproven
1296 route stays excluded.

The saved `vr_watch_offset.txt` calibration from the earlier successful
left-controller placement is restored as a local translation beneath the exact
left-hand parent. This affects placement only; all watch rotation still comes
from the completed hand hierarchy. The time is no longer transformed from its
flat HUD location. Its canvas is mapped directly onto the measured 6108 face
(local X/Z plane at the measured +Y surface), using the final physical-watch
matrix for its centre and both axes.

- Candidate: `Builds/left-watch-complete-native-cluster-20260823/d3d11.dll`.
- Deployed DLL SHA-256:
  `69C4EB6772B7CE7390D4A7E0A3597996CA591D84383217F196C9E905FC6F58DF`.

**Blue-indicator census and rigid-child diagnostic (2026-08-23):** video
`2026-08-23 10-30-32.mp4` showed that the 2628/750 additions moved the orange
physical part with the casing, but the conditional cyan darkness indicator was
still submitted at the right hand. The same test showed the hand-built casing
face mapping had collapsed the live time into an orange square, and the restored
legacy `vr_watch_offset.txt` translation displaced the casing without resolving
its wrist rotation.

This candidate makes three controlled changes. It restores the first proven
legible attached-time chain (`inverse(gameProjection)`, exact left-hand parent
delta, then per-eye projection/view correction); bypasses the legacy watch
translation while preserving the file and all left-hand calibration; and adds
two measurement-only logs. `VRPose watchrel` records the casing instance in
completed-hand-relative coordinates every 30 frames, proving whether the
visible rotation error enters before or after the instance buffer.
`VRPose watchcensus` records every small matrix-instanced viewmodel draw after
6108 on periodic frames, including index range, shader hashes and native
translation, so a headset run in which the cyan bar is illuminated can identify
the conditional draw absent from the saved light-scene RenderDoc capture.

- Candidate: `Builds/left-watch-blue-census-rigid-child-20260823/d3d11.dll`.
- Release x64 built successfully and deployed; source, candidate and game DLL
  hashes match.

**Rigid physical assembly, cyan pass and per-glyph face placement (2026-08-23):**
video `2026-08-23 10-42-17.mp4` and the diagnostic log resolved three separate
problems. The cyan bar is the conditional 60-index draw at start 585930/base
110311, shader pair `65C62A5148831C58 / 94ED7704592CAC87`; it carries the exact
same native instance as 6108 and is now admitted only with all of those live
discriminators plus the matrix-equality guard.

`VRPose watchrel` proved the previous `inverse(nativeHands) * nativeWatch` child
matrix changed during controller rotation, then settled around a repeatable
hand-relative transform. All four physical signatures now consume that fixed
watch-local transform beneath the exact completed left-hand instance, making
them one rigid assembly before any wrist-placement calibration. RenderDoc vertex
analysis independently confirmed the 6108 casing and digit components are
weighted to identity bone 3, ruling out downstream skin deformation.

Code review also found the 6108-only native-digit index split had accidentally
been applied to 2628 and both 750 draws, causing those smaller passes to read
beyond their own index ranges. Only 6108 is split now; every other physical pass
draws its exact original count. The custom HH:MM face mapping now preserves each
glyph's source-matrix horizontal translation rather than collapsing all five
characters onto the same position.

- Candidate: `Builds/left-watch-rigid-assembly-cyan-time-20260823/d3d11.dll`.
- Deployed SHA-256:
  `1EA6FA24BAE627962534DBC9489577D2D70E81F4ED744BB53ADE87200E40E4C4`.

**Exact-instance 3D time face (2026-08-23):** video
`2026-08-23 10-53-21.mp4` confirmed the cyan pass is inside the casing and the
physical watch rotates rigidly with the separated hand. The remaining orange
HH:MM glyphs maintained a stable but very large offset from the watch: their
parent rotation was correct, but converting Metro's generic 2D text matrix into
a physical plane did not share the casing shader's coordinate chain.

RenderDoc disassembly of event 118 records the complete casing vertex path:
packed position scaled by `12/32768`, identity watch bone 3, the three instance
rows, then `cb_main_matrices1.m_P`. There is no hidden model transform to
reconstruct. The existing four-digit 3D renderer is therefore re-enabled, but
its obsolete right-controller casing reconstruction has been replaced by the
exact final 6108 left-watch instance. Metro's five source glyphs are decoded
each frame and suppressed; the four digits are redrawn as seven-segment 3D
geometry with display X mapped to watch-local X, display Y to watch-local -Z,
and the face normal to local +Y. Both eyes use their own
`eyeProjection * viewCorrection` chain, matching the physical viewmodel.

- Diagnostic shader artifact: `GameReferences/watch_vs_chain.txt`.
- Candidate: `Builds/left-watch-exact-3d-time-face-20260823/d3d11.dll`.
- Deployed SHA-256:
  `FD7B5E5ED35B54546595F2C37D6191082779AF4AF1DE22407E4D9187A005A544`.

**Native-time restoration with independent calibration (2026-08-23):** the
exact-instance custom seven-segment renderer produced no visible digits in the
headset and has been disabled. Metro's native five-glyph `HH:MM` path is
restored to the previously visible chain: remove the game projection, apply the
completed left-hand parent delta, then apply each eye's projection and view
correction. This retains Metro's live glyphs, size, spacing and appearance.

The native time now has an independent saved calibration modeled on the proven
journal text calibration. `vr_watch_time_offset.txt` stores local position,
Euler rotation and scale; absence means identity defaults. Hold LT + both grips
for about one second to toggle time calibration. While active: left stick moves
right/up, left grip + left-stick vertical moves depth, right stick adjusts
pitch/yaw, right grip + right-stick horizontal adjusts roll, and right grip +
right-stick vertical adjusts scale. Toggling off saves. The distinct chord and
separate state guarantee these controls cannot modify the hand, physical watch,
or existing `vr_watch_offset.txt` values.

- Candidate: `Builds/left-watch-native-time-calibration-20260823/d3d11.dll`.
- Deployed SHA-256:
  `F6BD8E87A65152A03907FD6AF2E147793A412771D71708F9EECEA31950F41717`.

**Correction: exact 10:53 native-time baseline plus calibration (2026-08-23):**
the first native-time restoration above selected the wrong historical transform.
It used the earlier inverse-game-projection/hand-parent path, which changed the
clock's location, orientation and scale from the native time visible immediately
before the seven-segment experiment.

`BuildWristWatchScreenMatrix` now restores the exact per-glyph physical-face
mapping from `left-watch-rigid-assembly-cyan-time-20260823`. Its measured 6108
face point, pixel scales, canvas centre, source glyph offset and per-eye
projection chain are unchanged. With the calibration file absent/defaulted, an
explicit identity bypass sends that original face matrix directly to projection;
the calibration code therefore cannot perturb the baseline. Non-identity
adjustments are applied afterward to the complete native `HH:MM` group about one
shared face pivot, preserving glyph spacing while allowing placement, rotation
and scale adjustment. The independent LT + both-grips controls and file format
remain as documented above. The physical watch assembly and cyan routing were
not changed.

- `vr_watch_time_offset.txt` was absent at deployment, confirming an untouched
  identity baseline for the first headset test.
- Candidate:
  `Builds/left-watch-native-time-exact-baseline-calibration-20260823/d3d11.dll`.
- Deployed SHA-256:
  `D4D1C9751E9E78085DECCADAAD0F2C42E7746459E7F723348F83B900FCF40E56`.

**Rigid native-glyph layout (2026-08-23):** video
`2026-08-23 11-38-48.mp4` confirms that the native `HH:MM` face axes follow the
casing, but the text centre traces a small arc inside it during wrist rotation.
The casing and text already consume the same exact `sFinalWatch6108Instance`;
the remaining non-rigid input was `sourceCB[39]`, Metro's projected per-glyph
screen translation. Treating that changing projected value as a watch-local X
offset leaked the original right-hand watch pose into the reconstructed left
watch every frame. A placement calibration could align only one wrist angle and
could not remove this motion.

The five native glyph layout offsets are now sampled once, when each glyph first
appears with a valid final casing matrix, and then held rigid for the process.
Metro continues supplying the glyph texture/UV data, so the displayed time can
change normally; only the obsolete projected placement is frozen. All face
position, orientation and subsequent user calibration remain based on the exact
final 6108 casing matrix. The custom seven-segment renderer remains disabled.

- Video contact sheet:
  `GameReferences/video_watch_113848/rotation_8_20.jpg`.
- Saved-capture verification utility/output:
  `Tools/dump_watch_glyph_matrices.py` and
  `GameReferences/watch_glyph_matrices_all.txt`. The four captured digit draws
  share identical matrix axes but have translation deltas proportional to the
  projected X column, proving those values are projected layout coordinates.
- Candidate:
  `Builds/left-watch-native-time-rigid-glyph-layout-20260823/d3d11.dll`.
- `vr_watch_time_offset.txt` remained absent at deployment.
- Deployed SHA-256:
  `52E92D08642B5BAC25609886C2A84D5D6684593D33556E74AF5F7875F267EBD4`.

**Four native digits, immediate stereo, responsive scale (2026-08-23):**
video `Video Project 53.mp4` confirms the four-digit clock rotates rigidly with
the casing, but shows a mismatch on the apparent final glyph. The live log
identified the cause without inference: the first four captured local-X values
form the real clock sequence (`0.3057, 0.2996, 0.2923, 0.2862`), while the
accepted fifth generic draw jumps to `0.9035`. The colon is physical watch
geometry; Metro submits only four generic text digits. The fifth match was
unrelated HUD text and could also make the displayed value appear incorrect.

Watch UI matching now stops after exactly four digit draws. Watch glyphs are
also excluded from both deferred second-eye batching and single-pass folding,
just like the journal: every digit now takes the immediate twin path where
`BuildWristWatchScreenMatrix(1)` uploads its actual right-eye matrix before the
draw. This removes the stale/eye-0 matrix reuse behind the mismatch.

The watch-time scale control was active in the test, but the log shows it moved
only from `1.000` to `1.022` and back to `1.002`, explaining why no change was
perceptible. Right grip + right-stick vertical still means uniform bigger/
smaller while watch-time calibration is ON; its rate is increased from `0.006`
to `0.015` per frame. The last logged placement was not saved because the mode
remained ON when the process ended, so it was recovered to
`vr_watch_time_offset.txt` (`pos=-0.625,0.167,0`, near-identity rotation,
`scale=1.002`) rather than making the user repeat the placement.

Native glyph textures and UVs remain live; only their transforms are frozen.
Accordingly, the four-digit filter countdown should follow Metro's native
content when it replaces the clock, but still requires a headset/filter test.

- Video contact sheet: `GameReferences/video_watch_53/contact.jpg`.
- Candidate:
  `Builds/left-watch-native-four-digits-stereo-scale-20260823/d3d11.dll`.
- Deployed SHA-256:
  `8195FAB723B30FD3E2B58C742272072C1D8F732A055F920067AC49CAE03D803C`.

**Correction: retain colon and scale about clock centre (2026-08-23):** the
four-draw conclusion above was incomplete. Four sequential generic draws are
the numeric digits, while the live five-draw sequence submits the colon fifth
with a different projected origin. Limiting the block to four would remove the
colon. Matching is restored to five: the four numeric offsets remain frozen,
and the fifth punctuation glyph is placed explicitly halfway between digits two
and three instead of consuming its incompatible `sourceCB[39]` value. The
immediate per-eye exclusions remain in force for all five glyphs.

The user's direct observation that scale moved the clock rather than resizing
it was also correct despite the scale variable changing in the log. Calibration
was applying uniform scale around the casing face, but the reconstructed native
text has a large baseline offset from that face. Scaling therefore multiplied
the offset and translated the readout. The shared pivot is now the centre of the
four frozen digit positions. Rotation and scale operate around the readout
itself, while translation remains independent. The faster `0.015/frame` scale
rate remains.

Only glyph transforms are frozen. Metro's vertex UV/texture content remains
live, so a native switch from clock digits to filter countdown digits is not
intentionally changed by this work; that automatic state still awaits an actual
filter-area headset test.

- Reference-capture stage proof:
  `GameReferences/watch_native_text_stages/` and
  `Tools/save_watch_native_text_stages.py`.
- Candidate:
  `Builds/left-watch-native-colon-clock-pivot-scale-20260823/d3d11.dll`.
- Deployed SHA-256:
  `A9DA41D8E471056D969145D17685EDFE1C24F72A1E4EE9F88867B7EEDE1B37BB`.

**Rigid 3D live watch time (2026-08-23):** after the remaining native-text
glyph drift and final-digit stereo mismatch, the watch clock now uses the
dedicated 3D seven-segment renderer. Metro's four native glyph UVs remain the
data source, so the displayed numbers follow the value Metro submits (including
its automatic filter-countdown replacement); the native glyph quads themselves
are suppressed. A colon is generated explicitly as part of the same vertex and
index buffers, giving one rigid five-character `HH:MM` object.

The display model matrix is constructed once in the exact final 6108 casing
instance-local frame and used by both eyes; only the eye projection changes.
This removes all independent per-glyph transforms. Geometry is centred about
the complete readout before scale/rotation, and watch-time calibration now acts
in casing-local right/up/normal axes. The old native projected-text offsets are
not valid in this coordinate system, so they were backed up with the candidate;
the new baseline resets translation/rotation while retaining the user's chosen
`1.13` scale.

Native digit readback is limited to one complete batch every 30 frames instead
of synchronously reading every generic UI glyph every frame. This is more than
fast enough for the one-second clock/countdown changes and avoids continuous GPU
stalls.

- Candidate: `Builds/left-watch-rigid-3d-live-time-20260823/d3d11.dll`.
- Previous calibration backup:
  `Builds/left-watch-rigid-3d-live-time-20260823/vr_watch_time_offset.native-backup.txt`.
- Deployed SHA-256:
  `AB02AECCF12796A0F39664AD69AD82E48538547CD2197590B7334DEA42BC88B8`.

**3D clock value decoder correction (2026-08-23):** the first headset test
confirmed that the 3D readout is positioned correctly and rotates rigidly with
the watch, but its value was wrong. Direct inspection of the saved native watch
draws proved the decoder was using the wrong atlas: the watch texture is an
8-column, 128px-cell atlas with `0..7` on its first row and `8,9,:` on its
second, whereas `CaptureWatchDigit` was comparing U alone against the unrelated
ammo-font cell table. Invalid glyphs were rejected and valid accidental matches
were compacted into the wrong positions.

`MatchWatchDigit(u,v)` now decodes the measured watch atlas with both UV
coordinates, preventing the second-row cells from aliasing `0` and `1`.
The four positions are populated only by real numeric glyphs; the native colon
remains ignored because the 3D renderer supplies its own rigid colon. A concise
`VRPose watch decode: HH:MM` record is emitted for verification. The user's
saved 3D placement/rotation/scale was preserved unchanged.

- UV proof: `GameReferences/watch_glyph_uvs.txt` and
  `GameReferences/watch_glyph_atlas/ps_00.png`.
- Candidate:
  `Builds/left-watch-rigid-3d-live-time-atlas-fix-20260823/d3d11.dll`.
- Deployed SHA-256:
  `BB6D16A2E9723DDFC451AA99A4B9866FBBB81420210BD2253B50C362CD344853`.

**Authoritative clock trace and upright digit correction (2026-08-23):** a
read-only runtime hook identified Metro's real-life clock caller at
`metro.exe+0x2813EA`. The headset/filter test log also conclusively shows the
same native watch glyph stream switching from wall-clock `13:16` to filter
countdowns (`01:36`, then the selected filter resetting to `04:57` and counting
down through `04:00`, `03:57`, etc.). Thus the active native value stream does
already carry Metro's clock/filter selection; the apparent values in the 3D
display were a geometry-orientation problem, not a static clock source.

Video `2026-08-23 13-17-48.mp4` exposes that the custom segment plane was
vertically inverted. Symmetric digits (`0,1,3,8`) looked plausible, while `4`
looked upside-down and vertically asymmetric pairs (`2/5`, `6/9`) appeared as
the wrong character. The casing-local basis now maps display up to `+Z` and its
normal to `-Y`, flipping the complete readout about its already calibrated
centre. Position, scale and rigid casing rotation are unchanged. The temporary
`_localtime64` trace is no longer installed.

- Video review: `GameReferences/video_watch_131748/watch_crops.png`.
- Runtime caller proof is in the 13:18 `d3d11_log.txt` session.
- Candidate:
  `Builds/left-watch-rigid-3d-upright-digits-20260823/d3d11.dll`.
- Deployed SHA-256:
  `F4E811E8010D630BBEFC8EF253F96A3D85D35C75E6621CF6B507E5B7D0140378`.

**Immediate display and atomic countdown updates (2026-08-23):** the upright
digit test confirmed correct values/orientation but exposed two independent
timing defects. The display resource bootstrap was inside `DrawAmmoDisplay`
after weapon-view/weapon-affine checks, so the watch could not initialize until
a valid gun transform happened to exist. Shared shader/buffer creation now runs
before those weapon-only checks; the first complete watch value can therefore
draw on its first valid casing frame.

The `04:50 -> 04:40 -> pause -> 04:47` sequence had an exact decoder cause.
Digits 8 and 9 occupy the watch atlas's second populated row at captured
`minV=0.87890625`, but the initial table guessed `0.62890625`. On `04:49` and
`04:48`, the first three digits were accepted while the final digit was
rejected, partially overwriting the live array and retaining the prior trailing
zero. The second-row V is corrected, and each four-digit capture now decodes
into a temporary batch and publishes to the renderer only when all four digits
are valid. Partial/stale combinations can no longer appear.

- Candidate:
  `Builds/left-watch-immediate-atomic-countdown-20260823/d3d11.dll`.
- Deployed SHA-256:
  `EE7E20596D02F97A29A0674AB705AFF5B98C01BE145867D828D3ABE0BF0868CB`.

**Level-independent cyan visibility bar (2026-08-23):** after changing levels,
the cyan darkness indicator returned to the native right-hand watch while the
rest of the physical watch remained correctly attached. The runtime census
shows the cause directly: the conditional 60-index pass retains its unique
shader pair (`65C62A5148831C58 / 94ED7704592CAC87`) and the exact same native
instance matrix as the 6108 casing, but the level's combined mesh layout changed
its start/base from `585930/110311` to `453435/87390`.

The cyan discriminator no longer treats those level-specific buffer offsets as
identity. It still requires 60 indices and the exact shader pair, followed by
the existing same-frame matrix-equality check against the identified casing.
This preserves the strong watch-only guard while allowing Metro to repack level
mesh buffers. The working 3D time path and all saved calibration are unchanged.

- Candidate:
  `Builds/left-watch-cyan-level-independent-20260823/d3d11.dll`.
- Deployed SHA-256:
  `FB39AA24D46C48BAB90A47805F087F3B7C457B9886871BB4D0264C0FF5FBD936`.

**Unified watch-assembly calibration (2026-08-23):** the rigid completed-watch
transform is now adjustable as a single parent. The calibration is applied
after the fixed casing-to-hand transform and before that completed transform is
used by any child, so the 6108 casing, orange hardware, conditional cyan bar,
and custom 3D time all receive the exact same position and rotation. Rotation
changes the assembly basis about the watch origin rather than orbiting it around
the controller/hand origin.

The existing LT + right grip hold toggles this whole-assembly mode. While on:
left stick moves laterally/vertically; left grip + left stick Y moves in depth;
right stick rotates pitch/yaw; left grip + right stick X rolls around the watch
axis. Toggling the mode off saves all six values.

The new values live in `vr_watch_assembly_offset.txt` and begin at identity.
This is deliberately separate from legacy `vr_watch_offset.txt`, which remains
consumed by Metro's older casing shader path. The existing hand and independently
calibrated time-face files were preserved byte-for-byte, so the deployed build's
zero position is the placement established by the previous headset test.

- Candidate:
  `Builds/left-watch-unified-assembly-calibration-20260823/d3d11.dll`.
- Initial assembly calibration: `0 0 0 0 0 0`.
- Deployed SHA-256:
  `8A59D0118D619C286ECBDCFFD8D5F2D89FD3C406635F4E4C731243BE40AE38E3`.

**Watch wrist occlusion and walking-bob removal (2026-08-23):** after unified
assembly calibration, the custom time remained visible through the wrist when
the hand rotated over it. `DrawWatchDisplay` was explicitly binding a depth-
disabled `ALWAYS` state, so this was deterministic rather than a placement or
stereo problem. The injected readout now uses the active pass's depth function
with depth testing enabled and writes disabled. Its existing small face offset
keeps it above the casing, while the hand/wrist can correctly occlude it.

The completed watch-instance diagnostic remained rigid relative to the
controller during the walking test. Therefore the observed casing/hardware bob
was downstream of the shared parent, in Metro's animated b8 watch bone palette.
The first positively identified gameplay watch palette is now copied GPU-to-GPU
and rebound only around the already-guarded 6108/2628/750/cyan watch draws in
both eyes. This freezes the downstream cosmetic walking animation without a
CPU readback, while the controller-driven parent and all saved assembly/time/
hand calibration values remain live and unchanged.

- Preserved assembly calibration:
  `-0.054000 0.055500 0.106500 0.000000 0.050000 0.000000`.
- Candidate:
  `Builds/left-watch-depth-occlusion-no-bob-20260823/d3d11.dll`.
- Deployed SHA-256:
  `62C48F6BB2D8AB6F7E155693223E4488AD79502F330FCE4C55C355B1913E95B1`.

**Stable wrist anchor replacing failed palette freeze (2026-08-23):** headset
testing showed the palette freeze did not remove walking bob. The subsequent
runtime trace isolated the actual source: the hand-relative watch orientation
remained fixed, but its translation changed by approximately 10--20 cm during
Metro's walking animation. The hand's completed instance intentionally changes
translation every frame to cancel the animated local wrist centroid and keep
the *skinned visible wrist* on the physical controller. Attaching a rigid watch
to that compensated instance made the watch inherit the inverse animation.

The palette freeze has been removed. The hand and watch now use sibling
completed instances with the same live controller orientation and target wrist.
The hand instance continues using the live animated wrist landmark, preserving
the proven hand attachment. The watch instance freezes that local landmark on
its first valid gameplay frame and uses it only when solving translation. Thus
the entire rigid watch assembly remains controller-driven without inheriting
Metro's animation-cancellation displacement. The first frame is identical to
the previous hierarchy, preserving the user's calibrated placement.

The independently corrected time calibration was preserved:
`-0.000318 0.000303 -0.002759 -0.001678 -0.029611 -0.014000 0.956011`.

- Candidate:
  `Builds/left-watch-stable-wrist-anchor-20260823/d3d11.dll`.
- Deployed SHA-256:
  `FA3729B8D1730028B88D869103165CE4C07FDA3CDA34054CD6038D892C488D1A`.

**Chapter-independent watch-time sequence (2026-08-23):** chapter hopping
exposed the native orange `HH:MM` string on the HMD while the custom 3D readout
disappeared. The affected run still logged the unique watch-icon anchor every
frame, but logged neither fresh digit decodes nor source-glyph suppression.
The classifier was expiring eight draws after the icon; some chapters insert
more material/UI submissions before the five timer glyphs.

The watch block now remains armed only for the remainder of the icon's current
frame, consuming the same four generic-text digit draws plus colon and then
disarming immediately. The unique icon shader pair, generic-text shader pair,
six-index glyph shape, same-frame requirement and five-glyph limit remain in
place. This removes the chapter-specific draw-count assumption without turning
the match into a broad UI-text classifier.

The user's latest post-anchor watch and time calibrations were preserved:

- Assembly: `-0.907504 0.204000 -0.456001 -0.110000 -0.020000 0.000000`.
- Time: `-0.000318 0.000303 -0.002759 -0.001678 -0.029611 -0.014000 0.956011`.
- Candidate:
  `Builds/left-watch-chapter-independent-time-sequence-20260823/d3d11.dll`.
- Deployed SHA-256:
  `DD9B64E2D351B372FF5CDF7C5E5415A59545A44A4227684B7C82C2484ABBE00D`.

**Timer-atlas fallback for iconless chapter gameplay (2026-08-23):** the
same-frame search build still failed in the affected chapter. Its new runtime
trace proves why: frames 714--972 submitted the icon, decoded `14:36`, and
suppressed the native five glyphs normally; after gameplay began, the chapter
stopped submitting the icon entirely while continuing to draw the generic-text
timer. Thus no amount of widening the icon window could solve it.

The classifier now learns PS slot 0's original resource hash and dimensions
from a timer glyph that was positively identified by the unique icon. If the
icon later disappears, a fallback accepts only six-index draws using that exact
learned timer atlas under the existing generic-text shader pair. Fallback
matches reset at each frame and stop after five glyphs, preserving the native
four-digit-plus-colon grouping. This uses the timer's own content identity
without broadly suppressing Metro's other generic UI text.

- Candidate: `Builds/left-watch-timer-atlas-fallback-20260823/d3d11.dll`.
- Preserved assembly calibration:
  `-0.907504 0.204000 -0.456001 -0.110000 -0.020000 0.000000`.
- Preserved time calibration:
  `-0.000318 0.000303 -0.002759 -0.001678 -0.029611 -0.014000 0.956011`.
- Deployed SHA-256:
  `FA99B0E1A3658F6FA370721D5492E5219896EACCD6F3A252DA19A0F447FEF383`.

**Validated iconless-timer resources and persistent left-hand pose
(2026-08-23):** the first atlas fallback activated in the affected chapter but
never produced a valid digit decode. It had learned whichever generic glyph
texture happened to be last in the icon-anchored block. Timer resources now
enter the fallback set only after that same five-draw batch decodes as four
numeric glyphs plus its separator. Headset testing confirmed that this removes
the chapter-specific HMD timer while retaining the controller-attached 3D
display.

Weapon switching was then measured directly. Every tested weapon submitted the
same 12132-index hand geometry, vertex/index resources, material, and shaders;
only the 18 left-hand bone transforms changed. The headset-selected preferred
rows are embedded directly in the DLL, so every distributed copy uses the same
hand even when `vr_left_hand_pose.bin` is absent. A valid external file remains
an intentional override; a missing or invalid file uses the embedded pose. The
private palette is bound only for the isolated left-hand range in each eye and
Metro's palette is restored before the right hand, gun, watch, or the rest of
the stereo pass renders.

The watch remains on its original live-wrist parent rather than the frozen hand
landmark. This preserves the user's already calibrated watch hierarchy while
the visible hand keeps the saved pose across all weapons. Headset testing
confirmed the final combination: fixed hand pose and position across weapon
switches, with the complete watch visible and attached.

- Last headset-verified candidate:
  `Builds/persistent-hand-live-watch-parent-20260823/d3d11.dll`.
- Current distribution-ready embedded-pose candidate:
  `Builds/embedded-preferred-left-hand-pose-20260823/d3d11.dll`.
- Embedded pose payload SHA-256:
  `3E6018D6344146070E784354B92BAA519A0ABC382812DE785F3484142C381B3A`.
- Optional override: `vr_left_hand_pose.bin` (868 bytes including header).
- Preserved assembly calibration:
  `-0.907504 0.204000 -0.456001 -0.110000 -0.020000 0.000000`.
- Preserved time calibration:
  `-0.000318 0.000303 -0.002759 -0.001678 -0.029611 -0.014000 0.956011`.
- Last headset-verified DLL SHA-256:
  `9844431782D97D87D5F26BA8246F3CF0843EF61B2F09A0AA7D2122B95B66CEF2`.
- Current deployed embedded-pose DLL SHA-256:
  `4593D0D518746AECB965F3D9EC8DE7DAB014159E0B4826135DC15BE895A3F386`.

### Prologue left-hand attachment — first candidate (2026-08-23)

The prologue uses a separate 21,252-index combined arms/hands mesh. The saved
full-frame capture and its connectivity analysis already identify its exact
signature and triangle islands. The existing interception omits both sleeves
and retains both hands. The first controller-attachment candidate now submits
the two disconnected index runs belonging to the prologue left hand through
one shared left-controller private instance, preserving Metro's live prologue
bone palette because there is only one gun and no cross-weapon pose problem.
The right hand and existing sleeve suppression are unchanged. If controller or
instance data is unavailable during loading, the left hand falls back to the
previous native draw rather than disappearing.

The same saved frame proves that this prologue also submits the familiar watch
and native timer families: 6108, 2628, 750, the conditional 60-index cyan pass,
the exact watch-icon shader pair, and the generic native digit renderer. The
current candidate only logs those physical passes during the exact prologue
frame; it does not reparent the watch yet. The next step depends on the hand
test: if the instance-only hand tracks cleanly, derive and publish a dedicated
prologue wrist parent, then reuse the existing watch assembly and time/filter
decoder. Do not apply the normal gameplay embedded bone pose to this skeleton.

- Detailed handoff: `Notes/32-prologue-left-hand-and-watch.md`.
- Candidate: `Builds/prologue-left-hand-instance-20260823/d3d11.dll`.
- Candidate/deployed SHA-256:
  `A1040885397253F80828F50F1EFB081983798825E48CA0BFF1845905A762B5D3`.
- Build result: Release x64, 0 warnings, 0 errors.

**First prologue headset result and wrist-locked correction:** video
`2026-08-23 19-19-48.mp4` confirmed correct two-eye geometry and no obvious
physical-turn drift, but controller rotation produced a large rigid swing.
Runtime logs proved the left-controller substitution remained active, so this
was the expected wrong-pivot failure rather than a fallback. The video also
showed the watch still on the right controller. Live logs conclusively captured
the prologue's 6108, 2628, two 750 passes, exact cyan 60 shader pair, and native
icon-anchored clock glyphs (`19:23`).

The next candidate derives the actual prologue wrist from the saved mesh rather
than tuning a constant. It uses the 121 sleeve-facing vertices of the negative-X
hand component to generate weighted live-palette coefficients over prologue
bones 0, 48, 51 and 54. `BeginLeftHandBonePalette(true, true)` retains the live
prologue pose, constrains that evaluated wrist to the physical controller,
publishes a completed prologue hand/watch parent, and feeds one identical rigid
instance to both left-hand runs and both eyes. The normal embedded gameplay pose
is explicitly skipped for this skeleton.

- Candidate: `Builds/prologue-left-hand-wrist-locked-20260823/d3d11.dll`.
- Candidate/deployed SHA-256:
  `4109187947ECFB0A67D50CB54B2CBC5A95B1A4C93C66EFBDC7596BCF85E3C15D`.
- Build result: Release x64 succeeded with 0 errors. A broad header rebuild
  emitted eight pre-existing project/compiler warnings unrelated to this path.
- The first prologue positioning attempt changed the temporarily shared normal
  hand calibration. The altered value was preserved in the candidate folder,
  and the game file was restored to its exact pre-test gameplay value. Do not
  recalibrate the prologue hand until a separate calibration file is added.
# 2026-08-23 — Prologue hand calibration isolated; native watch parent restored

- The wrist-locked prologue test confirmed correct rotation and no
  physical-turn drift. The hand was in front of the controller, and the user
  deliberately did not calibrate because the test still shared normal
  gameplay's file.
- Log diagnosis of the invisible watch/time found the full native clock source
  still active (four digits plus colon). The replacement pipeline suppressed
  those source glyphs correctly, but the 6108 casing was transformed from
  native `(0.057, -0.165, 0.551)` to an off-wrist
  `(-0.329, -0.225, 0.151)` by gameplay's fixed watch-local matrix.
- Added independent `vr_prologue_left_hand_offset.txt` calibration, seeded with
  the proven normal-game values when absent. The prologue draw marks itself
  active, so the existing hand-calibration controls edit/save only that file.
  The existing `vr_left_hand_offset.txt` is never modified by this path.
- Prologue physical watch passes now inherit the exact native prologue
  hand-to-watch relationship under the completed stable wrist parent:
  `completedWrist * inverse(nativeHand) * nativeWatch`. Normal gameplay keeps
  its previously calibrated fixed watch-local implementation.
- Candidate:
  `Builds/prologue-left-hand-independent-calibration-watch-parent-20260823/d3d11.dll`
  (`D79C09867E629383BE35A17A8E99C1760CD6BFE8A3647BEC9BDF9DEC9FD65C5A`).
  Release x64 succeeded with 0 errors and 1 pre-existing C4250 warning. Deployed
  to the game directory; verified normal hand file remained unchanged.
# 2026-08-23 — Independent prologue watch assembly calibration

- Reviewed `2026-08-23 19-44-39.mp4`: the prologue hand placement and rotation
  are correct; watch casing, cyan hardware and time are together, but orbit far
  ahead of the wrist.
- Runtime `watchrel` traces prove the relative rotation is stable. The remaining
  error is the native prologue child translation, approximately
  `(0.0572, -0.1650, 0.5840)` (0.61 units from its parent).
- Added `vr_prologue_watch_assembly_offset.txt`, selected automatically by the
  existing watch-calibration chord while the opening mesh is active. Default
  translation cancels that measured lever arm; subsequent position/rotation
  adjustments affect the entire prologue watch/time assembly only.
- Preserved the user's newly saved prologue hand calibration exactly:
  `-0.088000 0.208000 -0.498001 0.999999 -1.049999 0.000000`.
- Candidate `Builds/prologue-left-watch-independent-calibration-20260823/d3d11.dll`,
  SHA-256 `5475E6EAD5465CDCB4F99D8BA0B39CE35193419482166943FBEADE22D2E682C5`.
  Release x64: 0 errors, one pre-existing C4250 warning; deployed.
# 2026-08-23 — Prologue watch moved from animated to rigid wrist pivot

- Reviewed `2026-08-23 19-53-21.mp4`. Once placed near the wrist, the prologue
  watch visibly swung during controller rotation despite a nearly constant
  instance-level `watchrel` matrix. The prologue's native hand-to-watch local
  transform therefore contains Metro's animated viewmodel pivot and is not a
  valid rigid-child baseline.
- Prologue watch assembly now uses the same measured `fixedWatchLocal` rigid
  baseline that solved normal gameplay, with its independent prologue
  calibration applied on top.
- Mathematically migrated the user's saved watch pose into the rigid baseline:
  position `(-0.159300, -0.019100, -0.271200)`, rotation
  `(0.292923, -0.282777, -0.218408)`. Preserved the original file as
  `vr_prologue_watch_assembly_offset.native-pivot-backup.txt`.
- Log evidence shows the previous prologue hand save was loaded correctly as
  `(-0.088, 0.208, -0.498)`, then the user's second calibration changed/saved
  it as `(0.058, 0.204, -0.498)`. The newer file remains unchanged.
- Candidate `Builds/prologue-left-watch-rigid-pivot-20260823/d3d11.dll`, SHA-256
  `80E1855301060433FB6CEDE7AA7A6A1EF03933E496102B99F7EF5A0B158FA2F8`.
  Release x64: 0 warnings, 0 errors; deployed.
# 2026-08-23 — Deterministic prologue hand placement across launches

- Headset report confirms the rigid-pivot prologue watch now rotates perfectly
  with the hand.
- Diagnosed repeated hand repositioning: the calibration file loaded correctly,
  but the hand's controller-local authored basis was recaptured from the
  startup tracking anchor each process. Identical saved values therefore meant
  a different pose after every restart.
- Prologue now uses a fixed identity authored basis under the absolute live
  controller rotation. Normal gameplay retains its existing captured basis.
- Fixed the prologue watch's stable model-space wrist reference to the verified
  `(-0.51268, -0.00885, 0.00092)`, preventing first-animation-frame variance.
- This basis transition requires one final calibration; subsequent saved values
  are deterministic across process launches.
- Preserved current files unchanged: hand
  `(0.014, 0.388, -0.288, 0.999999, -1.049999, 0)` and watch
  `(-0.450301, 0.247900, -0.205200, 0.522923, -0.332777, -0.148408)`.
- Candidate `Builds/prologue-left-hand-deterministic-basis-20260823/d3d11.dll`,
  SHA-256 `8BE785D2F4F6B9B6936327B3220753613C08A997DC45B253E754B419A395B6CA`.
  Release x64: 0 warnings, 0 errors; deployed.
# 2026-08-23 — Prologue hand/watch persistence verified

- User tested the deterministic-basis build after a full game restart and in a
  brand-new game.
- The prologue hand and complete watch assembly remained at their saved
  positions in both cases.
- Watch rotation remains perfectly matched to the hand.
- `Builds/prologue-left-hand-deterministic-basis-20260823/d3d11.dll` is now the
  verified prologue baseline.
# 2026-08-23 — Prologue implementation handoff expanded

- Expanded `Notes/32-prologue-left-hand-and-watch.md` to the same handoff depth
  as the normal separated-hand/watch note.
- Recorded the final draw split, prologue skeleton/bone selection, anatomical
  wrist coefficients, deterministic controller basis, stereo invariants,
  complete physical-watch and live clock/filter-timer routing, calibration
  controls, exact final files/values, rejected approaches, recovery commit/DLL
  hash, and verification checklist.
- Added explicit future runtime-toggle guidance: original-hands mode must branch
  inside the 21,252-index interception so both sleeves remain removed while the
  separated left-hand palette, watch parenting, and custom-clock routing are
  disabled.
- Recorded a release-critical caveat: the exact final prologue hand/watch
  placement currently depends on shipped calibration files unless those values
  are embedded as the missing-file defaults before distribution.
# 2026-08-23 — Final prologue/watch calibration defaults embedded

- Embedded the headset-verified prologue left-hand placement
  `(-0.192000, 0.166000, -0.436001; 0.190000, -1.009999, 0.000000)`.
- Embedded the headset-verified prologue watch placement
  `(-0.450301, 0.247900, -0.205200; 0.522923, -0.332777, -0.148408)`.
- Embedded the shared normal/prologue custom time-face placement and scale
  `(-0.000318, 0.000303, -0.002759; -0.001678, -0.029611, -0.014000; 0.956011)`.
- Updated both startup initializers and malformed-file fallbacks. Existing user
  calibration files still override the embedded defaults.
- A clean distribution now reproduces the verified placement without requiring
  any of the three calibration files.
- Release x64 succeeded with 0 errors and produced/deployed
  `Builds/embedded-final-hand-watch-defaults-20260823/d3d11.dll`, SHA-256
  `FF3BF895EDA29630C0CE994724FF9856721EDBC77742350600063B154AF004E0`.
# 2026-08-23 — Hand/watch setup controls removed from release input

- Disabled the three temporary setup entry chords for left-hand placement,
  complete-watch placement, and watch-time placement now that their final
  headset-verified values are embedded.
- Preserved their loaders/getters and calibration-file compatibility for
  existing installations and possible future development; only user-facing
  runtime adjustment input is disabled.
- Re-enabled the established weapon calibration on left trigger plus left grip,
  which the temporary left-hand setup mode had displaced.
- Journal calibration remains unchanged and retains precedence while its
  confirmed draw is visible.
- Release x64 succeeded and the resulting DLL was archived/deployed as
  `Builds/release-hand-watch-controls-cleanup-20260823/d3d11.dll`, SHA-256
  `6AFBEB92B5CA62D8DA59D66DCFD7ACE03960EF647D96DE551477CB2B5900358A`.
# 2026-08-23 — Lighter left-hand attachment investigation started

- Created branch `codex/left-hand-lighter-attachment` from merged master.
- The requested solid lighter and its HMD-locked flame have no existing
  lighter-specific capture/signature. Older flame notes concern an unrelated
  ambient world flame and must not be reused as identification evidence.
- Added an automatic settled-frame capture keyed to the synthetic lighter
  action. Beginning with the lighter off, toggle sequence 1 captures lighter-on
  after 90 frames; after the user waits and toggles it away, sequence 2 captures
  the same scene lighter-off.
- Captures include render targets, constant buffers, vertex/index buffers, and
  formatted buffer dumps so the physical lighter and independent flame layers
  can be identified by a same-scene differential before any transform is
  applied.
- Diagnostic artifact: `Builds/lighter-differential-capture-20260823/d3d11.dll`,
  SHA-256 `7C336E5FFADDA24305390772078FB6D0812A82D6CDAE20B7C2DEDA7FBC533757`;
  Release x64 succeeded and the same hash was deployed to Metro.

## First lighter capture result

- The right-stick-up lighter action was detected correctly: the log recorded
  sequences 1 and 2 and created both requested directories.
- Both directories contained only an empty `ShaderUsage.txt`. The capture was
  armed by `DrawIndexed` late in its frame and Present ended the analysis before
  a dump-capable draw passed through the normal analysis tail.
- `MaybeStartLighterFrameAnalysis` is now called from all four draw entry types,
  matching the established muzzle diagnostic coverage. This should arm on the
  first draw of the settled target frame rather than a late viewmodel draw.
- Corrected artifact:
  `Builds/lighter-differential-capture-v2-20260823/d3d11.dll`, SHA-256
  `04242330711751C07485C8296779AE4AA743337D286785327837A253884888F3`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Second lighter capture result and direct census fallback

- Both corrected sequence folders were still empty. The toggle detector again
  worked exactly, but this proves the frame-analysis subsystem cannot be armed
  reliably from inside Metro's active draw on this build.
- Disabled the automatic frame-analysis attempt.
- Added a bounded direct draw differential in `d3d11_log.txt`. Before the first
  lighter action it records a stable lighter-off baseline across all four draw
  APIs. During odd/on toggle sequences it logs each signature absent from that
  baseline, including draw type, geometry range, instances, VS/PS hashes,
  layout/class, pass, viewport, matrix-buffer offset, and native translation.
- The next even/off toggle logs set totals. This path can identify both ordinary
  solid lighter meshes and non-matrix/instanced flame layers without relying on
  frame-analysis activation.
- Direct-census artifact:
  `Builds/lighter-direct-differential-20260823/d3d11.dll`, SHA-256
  `7D92EDE9C26A405B1BE897DE3F909F456D02880FE526ADDA8CDBF2D8960D2832`;
  Release x64 succeeded and the same hash was deployed to Metro.

## First direct-census result and relevance filter

- The unfiltered lighter-off baseline reached its 24,000-signature cap and the
  lighter-on set reached 32,766 signatures because pooled world geometry ranges
  churn as the scene streams. The 600-line new-signature budget was exhausted
  by world draws.
- Only eight logged entries were relevant to the requested object: five
  viewmodel entries and three known non-matrix particle batches.
- The `162`, `180`, `16251`, `4320`, and `1296` viewmodel cluster matches the
  previously captured ordinary gun/hand/watch block. In particular `16251` is
  the confirmed SMG body, so that cluster must not be reparented as the lighter.
- The census now rejects everything except the viewmodel pass, known non-matrix
  particle layouts, and offscreen-effect passes before inserting into either
  set. World streaming can no longer consume its baseline or log capacity.
- Relevance-filtered diagnostic artifact:
  `Builds/lighter-relevant-differential-20260823/d3d11.dll`, SHA-256
  `5E692C76A9DEB56112075BC44AA8E4FC995662C85CFD7271AE2BE0249A67F0F0`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Relevance-filtered result and visual identity check

- The filtered run still logged 800 entries, but normalized grouping separated
  the recurring render-target/effect traffic from two plausible lighter paths.
- A single 2,499-index skinned viewmodel draw appeared immediately after the
  lighter toggle: VS `79BA9F1ECFAF0CA1`, PS `6597099FD3B895DB`.
- A changing 6-index non-matrix instanced viewmodel batch used VS
  `E1FDE311D1E35018` and pixel shaders `C7170B0AB238E3EC` /
  `252FCE3C8790BFBC`. This is the plausible independent flame path; instance
  count changes as its particles spawn and expire.
- Added a temporary exact-signature suppression while the synthetic lighter
  state is on. The next headset run should show whether the 2,499-index draw is
  the solid lighter and whether the two instanced variants contain its flame.
- Visual-check artifact:
  `Builds/lighter-candidate-visual-check-20260823/d3d11.dll`, SHA-256
  `3069BD5B19F4C7B5FA5DADCB16BA1D1C7CF77861F47BF96704CAB89F49F83910`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Solid lighter confirmed; flame differential normalized

- The visual-check log continuously suppressed the exact 2,499-index draw
  while diagnostic sequence 1 was active. The user's report of an invisible
  item animation followed by the lighter becoming visible on the next toggle
  confirms this is the solid lighter body.
- Neither proposed instanced flame signature was suppressed, so the earlier
  `E1FDE311D1E35018` candidates are not accepted as the flame.
- The remaining differential inflation was caused by including pooled
  start/base offsets and changing particle instance counts in identity keys.
  The key now uses only draw API, primitive count, VS/PS hashes, layout class,
  viewmodel classification, and offscreen-effect classification. Live offsets
  and instance count remain in the log as evidence but no longer create false
  unique objects.
- Disabled candidate suppression for the next capture so the lighter and flame
  remain normally visible while the normalized census runs.
- Normalized-differential artifact:
  `Builds/lighter-normalized-differential-20260823/d3d11.dll`, SHA-256
  `F1F16CD8A464A868D43EA777CC767C4BE7DD59B2D9C36032248A4321E8AABE5D`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Normalized result and first solid-body attachment

- The normalized baseline contained only 72 stable signatures. While the
  lighter was visible, 57 signatures were observed and exactly three were
  absent from the lighter-off baseline:
  - solid body: 2,499 indices, VS `79BA9F1ECFAF0CA1`, PS
    `6597099FD3B895DB`, skinned instance-matrix viewmodel path;
  - flame material A: 6 indices, VS `E1FDE311D1E35018`, PS
    `C7170B0AB238E3EC`, non-matrix instanced viewmodel path;
  - flame material B: same draw identity with PS `252FCE3C8790BFBC`.
- Added an exact solid-body route before generic weapon substitution. On its
  first visible frame it freezes `inverse(nativeLeftHand) * nativeLighter`.
  Every later frame computes `completedLeftHand * fixedLighterLocal`, binds
  that eye-independent instance for both stereo draws, and preserves the
  lighter's native skin palette. This removes right-hand/controller animation
  from the body while retaining its authored hand-relative placement.
- Also records `finalLighter * inverse(nativeLighter)` for the separate flame
  path. The flame is not transformed in this first attachment build; it will
  be handled after verifying that the solid body follows and rotates with the
  completed left hand correctly.
- Solid-body attachment artifact:
  `Builds/lighter-solid-left-hand-parent-20260823/d3d11.dll`, SHA-256
  `E3E2BC51A910E04F3DE4BC319BE5D6E6C071938D1FE32496D63CA0113923C7F8`;
  Release x64 succeeded and the same hash was deployed to Metro.

## First solid-body video result

- Video `2026-08-23 21-45-20.mp4` proves the body receives the left-hand
  rotation, but it orbits far above/in front of the hand. The logged final
  instance translation moves by several tenths of a metre as the controller
  rotates, confirming a parent-space lever arm rather than stereo drift or a
  failed rotation.
- Cause: `inverse(nativeLeftHand) * nativeLighter` retained Metro's authored
  right-side lighter translation. That is not a useful left-hand local
  placement and reproduces the same swinging-pivot failure seen during watch
  development.
- The fixed lighter local now retains only the native relative 3x3 basis and
  zeros its translation, placing its instance origin directly at the completed
  left-hand origin. Fine hand-local placement will be calibrated only after
  this pivot-centred behavior is confirmed.
- The video also clearly shows the flame remaining at its old HMD-relative
  location, which is expected because the flame route has not yet been changed.
- Zero-lever-arm artifact:
  `Builds/lighter-solid-zero-lever-arm-20260823/d3d11.dll`, SHA-256
  `D447CF2988331911052F36E2195282F247C0F6CFA2B0A98EEE4F95BEFC480985`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Zero-lever-arm video result and skinned-bone scan

- Video `2026-08-23 21-57-14.mp4` shows the large inherited instance offset is
  removed, but the visible body still pivots around a point outside the mesh.
- This confirms the 2,499-index shader's skinned output is downstream of the
  instance-only correction. An instance translation alone cannot establish the
  visible lighter's pivot.
- Added a one-shot scan on the already exact body signature. It reads only the
  draw's indexed vertex range and logs weighted `BONES`/`WEIGHTS` usage from
  the captured 32-byte skinned layout. The next implementation will correct
  precisely those palette rows, using the proven hand controller affine, and
  will leave every other hand/weapon bone untouched.
- Bone-usage scan artifact:
  `Builds/lighter-bone-usage-scan-20260823/d3d11.dll`, SHA-256
  `5FC60F388C4523155A517160B548675EDE2A9EDE58E6A7AD6EC5F9438973E701`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Lighter bone result and centre-anchored palette path

- The live scan found 750 unique vertices and exactly three weighted bones:
  base rows 0, 3 and 6 (weights 119,340 / 48,960 / 22,950).
- The first instance-only implementation is replaced by a skinned-aware path.
  At first sight of the exact mesh it derives a vertex-mean linear coefficient
  for each of those bones from the shipped index/vertex data. Each frame it
  evaluates the visible lighter centre against Metro's live `b8` palette.
- The completed left-hand 3x3 basis is retained, but translation is solved so
  the evaluated visible centre lands exactly at the completed hand origin.
  This removes both the native right-hand lever arm and the downstream skinned
  lever arm while preserving Metro's live lid/opening animation.
- The live palette is copied to a private `b8` buffer for the lighter only and
  rebound identically for eye 1. No bone rows belonging to the separated hand,
  gun, watch, NPCs, or world meshes are modified.
- Skinned-centre attachment artifact:
  `Builds/lighter-skinned-centre-anchor-20260823/d3d11.dll`, SHA-256
  `45BC3D72F224E0711FE6BDE4D186FEFDBA8CBE317AFE30FEA1F2ABA6EB2DEEBB`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Verified rigid lighter pivot and dedicated placement calibration

- Headset result from the skinned-centre build: the lighter is well behind the
  hand, but now rotates correctly. This verifies that the visible-centre solve
  removed the hidden skinned lever arm; the remaining error is a constant
  hand-local placement, not another pivot or tracking problem.
- Added `VRPose::MarkLighterBodyActive()` to the exact 2,499-index draw. The
  input path accepts the new calibration chord only while that exact body was
  rendered within the last second (or while calibration is already active), so
  it does not interfere with ordinary weapon calibration when the lighter is
  put away.
- Temporary lighter calibration toggles with **LT + right grip held for about
  one second** while the lighter is visible. It saves to
  `vr_lighter_offset.txt` when the same chord toggles the mode off:
  - left stick: hand-local X/Y position;
  - left grip + left stick up/down: hand-local depth/Z;
  - right stick: pitch/yaw;
  - right grip + right stick left/right: roll.
- The calibrated Euler rotation and position are applied around the measured
  visible lighter centre. In matrix terms, the target centre is
  `completedHand * calibrationPosition`, while the final basis is
  `completedHand3x3 * calibrationRotation`; translation is then solved by
  subtracting that final basis times the evaluated live skinned centre. Thus
  rotating during calibration cannot recreate the former orbit/swing.
- Lighter calibration temporarily preempts locomotion, turning, and weapon
  calibration input. The established weapon chord remains unchanged outside
  this exact lighter-only mode.
- Placement-calibration artifact:
  `Builds/lighter-body-calibration-20260823/d3d11.dll`, SHA-256
  `C68FB2BA251625B7D052963640006FA37045A362AF2A8B5A4C84631086DAEF87`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Verified lighter placement and first flame attachment

- The user positioned the body and confirmed that it continues to rotate
  correctly. The headset-saved `vr_lighter_offset.txt` values are:
  `position=(0.111402, -0.105089, 0.682597)`,
  `rotation=(1.058952, -0.266553, 0.702228)` radians. These remain file-backed
  during development and should be embedded after final flame verification.
- Attached both exact six-index flame material variants (VS
  `E1FDE311D1E35018`, PS `C7170B0AB238E3EC` / `252FCE3C8790BFBC`)
  through the completed lighter transform rather than a guessed flame offset.
- `PatchMappedVRObjectData` now retains a CPU-side copy of the most recently
  mapped object's unchanged `m_W`. At the exact flame draw, the route computes
  `finalFlameW = (finalLighter * inverse(nativeLighter)) * nativeFlameW`.
  This preserves Metro's authored emitter-to-wick relationship and live
  particle-instance animation while removing its old HMD/right-side parent.
- The flame draws use a private 208-byte `cb_main_matrices0`. `m_W`, `m_iW`,
  `m_WV`, and `m_WVP` are rebuilt from the final flame transform. Eye 0 and
  eye 1 are submitted explicitly, with the private buffer recomputed after
  entering the twin pass so the second eye cannot inherit eye 0's matrix.
- Flame-parent artifact:
  `Builds/lighter-flame-left-hand-parent-20260823/d3d11.dll`, SHA-256
  `9CAC5933F3B0C5AF90D8E0011B04A0007EFE2A4729819B02C31296C257258113`;
  Release x64 succeeded and the same hash was deployed to Metro.

## First flame result: diagnosed space mismatch and missing dynamic layers

- Video `2026-08-23 22-34-46.mp4` shows no main flame at the lighter while a
  partial flame/smoke effect remains HMD-locked. Frame-by-frame review confirms
  the lighter body itself remains correctly placed.
- The runtime log makes the missing flame deterministic: the first flame route
  produced `finalW` translations around 100-150 metres (examples include
  `(-29.966,-33.740,146.274)` and `(134.925,-4.502,71.678)`). The flame's
  unchanged `m_W` is level/world-space, whereas `finalLighter *
  inverse(nativeLighter)` is a viewmodel/view-space delta. Multiplying that
  delta directly into `m_W` mixed coordinate spaces and moved the two
  six-index materials far outside the visible lighter.
- Corrected ordering is now: `nativeWV = eyeView * nativeFlameW`, then
  `correctedWV = lighterParentDelta * nativeWV`, followed by that eye's
  projection. `m_W`/`m_iW` remain a coherent native world pair; the exact
  flame shaders declare those fields unused and consume only `m_WV`/`m_WVP`.
- The live differential also found two lighter-only dynamic draws which were
  absent from the earlier stable normalized census:
  - 18 indices, VS `77840E246BBF6E55`, PS `8281F4D8AE4065B0`;
  - 24 indices with the same shader pair.
  Their shader disassembly confirms the same `cb_main_matrices0` WV/WVP path.
  Both are now routed through the corrected per-eye lighter-flame helper, gated
  by exact count/hash, the offscreen-effect pass, gameplay, and a current
  completed-lighter transform. These account for the visible core/smoke that
  remained on the HMD.
- Corrected complete-flame artifact:
  `Builds/lighter-flame-viewspace-complete-20260823/d3d11.dll`, SHA-256
  `FA492F705EA54274659438B79692CDDCFBC0E897A579FB682752C23D440E1E43`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Both-eye flame video: IPD-order fix and variable smoke batch

- Side-by-side video `Video Project 55.mp4` confirms the main flame now follows
  the lighter, but separates between the eyes at particular wrist rotations.
  Other effect layers remain fixed to the HMD.
- Cause of the stereo split: the prior build computed each eye's native WV and
  then applied the rotating lighter-parent delta. That rotates the eye's IPD
  translation along with the flame, so binocular separation changes with wrist
  orientation. The correct hierarchy is shared headset-centred object pose
  first, eye translation last.
- The flame helper now removes eye 0's IPD offset from the cached view to build
  a centre view, computes `nativeCentreWV`, applies the lighter delta once to
  obtain `correctedCentreWV`, and only then subtracts the selected eye's offset
  before projection. Both eyes therefore view one identical flame pose with
  ordinary fixed-IPD parallax.
- The new live differential reports the previously HMD-locked effect as 12
  indices in this run, whereas it was 18/24 in the preceding run. It is one
  variable-size particle batch, not three fixed mesh signatures. The route now
  accepts the exact VS/PS pair for any batch size in the offscreen-effect pass,
  but the shared helper additionally requires a current completed lighter and
  rejects emitters whose native centre-space distance exceeds 3 game units.
  This admits the near-eye lighter core/smoke without moving distant world
  effects that reuse the particle shader.
- Centred-stereo/dynamic-batch artifact:
  `Builds/lighter-flame-centred-stereo-dynamic-batch-20260823/d3d11.dll`,
  SHA-256
  `CA45017CD7157EE3BE5C2075934FACF954B82E7AE96D53C48D32E10AD9885A13`;
  Release x64 succeeded and the same hash was deployed to Metro.

## No split, but live HMD emitter motion and rejected dynamic vertices

- Video `2026-08-23 22-55-41.mp4` verifies the centre-first IPD correction:
  the main flame no longer splits. It is not centred on the wick, however, and
  still changes position with headset motion. The other flame/smoke layers
  remain HMD-attached.
- Logs show the lighter's native instance settles near
  `(-0.122,-0.107,0.650)`, while the supposedly native flame WV varies with
  head movement (for example Y from `0.037` to `0.377`). The six-index emitter
  matrix itself is therefore regenerated from the HMD; retaining its live
  native pose necessarily retains head motion even after correct stereo.
- The exact six-index main emitter now captures one rigid local transform on
  its first valid draw:
  `fixedFlameLocal = inverse(nativeLighter) * nativeCentreFlameWV`.
  Every subsequent eye/frame uses
  `correctedCentreWV = finalLighter * fixedFlameLocal`, followed only by the
  selected eye offset and projection. This removes all future HMD input while
  preserving Metro's initially authored emitter-to-wick relation. A separate
  flame-local calibration can be added if the resulting fixed relation still
  needs a small wick-centre adjustment.
- The dynamic core/smoke draw stores CPU-built world positions in its vertex
  stream and commonly uses identity `m_W`. The previous 3-unit test examined
  that irrelevant matrix origin, rejected the draw, and explains the reported
  lack of change. The exact shader/pass route now bypasses that distance test;
  it remains gated on active gameplay and a current completed lighter.
- Rigid-emitter/dynamic-accept artifact:
  `Builds/lighter-rigid-flame-local-dynamic-accept-20260823/d3d11.dll`, SHA-256
  `02139B5AD4F02B78F857BF885D9D9A5192C9BD21EEC8898A1BF58999EDEC89F8`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Matrix stayed rigid: flame centre proven to live in instance vertices

- Follow-up headset report: the main flame still moves with the headset.
- The new log proves the frozen matrix path was active: one local transform was
  captured (`T=(-0.0063,0.1569,0.0048)`) and later `finalWV` values were built
  from the final lighter, while the logged native emitter continued changing.
  Therefore the remaining HMD motion cannot come from `cb0`.
- Disassembly of exact VS `E1FDE311D1E35018` shows the actual particle centre
  comes from `inst0.xyz` (`v1.xyz`) in per-instance vertex-buffer slot 1. The
  exact layout is 64-byte stride: `inst0` at byte 0, `inst1` at 16, `inst2` at
  32, then packed animation/UV fields through byte 63. The shader adds this
  centre to its generated quad before multiplying by `m_WVP`; freezing only
  `m_WVP` could never remove a centre that the engine rewrites from the HMD.
- On the first exact flame draw, the route now performs one 64-byte readback,
  transforms `inst0.xyz` through the native emitter WV into headset space and
  then through `inverse(nativeLighter)` into lighter-local space. Only those
  first 12 bytes are replaced. Size, orientation, colour, UV and animation
  payload remain native. A private 64-byte instance VB is then bound for both
  eyes, while `m_WVP` is built directly from the final lighter plus the selected
  eye offset. No live HMD emitter position remains in either the matrix or
  vertex position path.
- The secondary core/smoke batch similarly carries changing CPU-built absolute
  positions in ordinary vertices. It is now suppressed under its exact
  shader/pass signature while the lighter is active, rather than leaving a
  visibly HMD-attached duplicate. Rebuilding optional smoke as a local custom
  effect can be considered after the main flame is verified.
- Instance-centre-local artifact:
  `Builds/lighter-flame-instance-centre-local-20260823/d3d11.dll`, SHA-256
  `8CA7176B385542B8DC02FB067E5358BA98212039BCE26D0E2F9089C3CCCC1CAE`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Instance rewrite rollback and complete particle-input diagnosis

- Video `2026-08-23 23-10-42.mp4` disproved the preceding instance-centre-only
  approach: the flame became a huge stretched beam and still responded to the
  HMD. Replacing only `inst0.xyz` is invalid because the same 64-byte instance
  contains the remaining orientation, size and billboard data; changing its
  centre without changing those coupled fields creates an inconsistent
  particle. The deployed DLL was immediately rolled back to the normal-looking
  `02139...` rigid-matrix build while diagnostics were prepared.
- Smoke suppression was also removed. The smoke has gameplay value, and the
  diagnostic showed the alleged lighter smoke shader is reused by ambient
  level effects, including matching draws before the lighter was activated.
  It is therefore unsafe to suppress or reparent every draw with that shader.
- Diagnostic artifact
  `Builds/lighter-complete-particle-input-diagnostic-20260823/d3d11.dll`,
  SHA-256
  `0191F2885F829FB7F5B962599A245D5C7B1E9324F1D52F62D272EDF8001CC462`,
  restored the `02139...` visual behavior and recorded the complete main-flame
  instance data, `eye_position`, all seven `cb_particles` vectors, and sampled
  bounds for the dynamic smoke/core vertex batches.
- The controlled head-then-controller capture proves two independent causes:
  - The actual main flame is the only captured matching draw with
    `particles_flags=(0,1,1,0)` and short emitter lifetime (`particles_T.w`
    approximately `6.55`); unrelated matching particle draws use
    `(1,0.5,1,0)` and `particles_T.w=100`.
  - `particles_flags.z=1` selects the shader's camera-facing path, which reads
    `eye_position` and rebuilds billboard axes from the HMD. This explains the
    remaining head response after the flame matrix itself was made rigid.
  - Dynamic VS `77840E246BBF6E55` receives CPU-generated world-space vertices
    and identity `m_W`. Sampled ambient bounds were around world Z=128 while
    the camera was around Z=145, approximately 16 metres from the player.
    Shader identity and index count cannot distinguish those effects from the
    lighter smoke.

## Positive flame/smoke classification build

- The main flame route now reads `cb_particles` and only accepts the verified
  lighter signature `flags=(0,1,1,0)` with `0 < particles_T.w < 20`. It copies
  the native constants into a private 112-byte buffer and changes only
  `flags.z` to zero. That selects Metro's emitter T/R-axis branch instead of
  the `eye_position` billboard branch; native animation remains intact and the
  rigid lighter-parent matrix supplies wrist rotation. All other draws using
  the same VS/PS fall through untouched.
- The dynamic smoke/core route now reads the small referenced vertex range,
  computes its world-space bounds centre, transforms it into centre-eye view
  space, and compares it to the current native lighter instance. Only geometry
  within 1.0 game unit of the actual lighter is reparented. Distant ambient
  effects sharing the shader are never changed. Classification is logged as
  `VRPose lighter dynamic classify=LIGHTER|ambient` with both positions and
  distance for verification.
- Positive-classification artifact:
  `Builds/lighter-particle-positive-classification-20260823/d3d11.dll`,
  SHA-256
  `CB5ECA48BDB200242D4A896D6814E393BD7E4581F60817D1B98870C41BBB596D`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Sustained flame layers invalidate the ignition-only signature

- Test of `CB5E...` showed the complete visible flame returned to the HMD. The
  log explains why: the `flags=(0,1,1,0), T.w=6.55` signature occurs only for
  the first ignition particle. Sustained layers immediately use
  `flags=(1,0.5,1,0), T.w=100`; rejecting those draws allowed Metro's original
  HMD-relative render path to take over after ignition.
- Main six-index particle draws are now accepted by spatial identity instead:
  their live `m_W` origin is converted to centre-eye view space and must be
  within 1.0 game unit of the current native lighter instance. This includes
  ignition and sustained layers while rejecting matching world effects.
- Accepted layers receive the private `cb_particles` copy with camera-facing
  `flags.z` disabled and use the live lighter parent delta rather than one
  shared frozen emitter matrix. That preserves the separate native layer
  offsets while removing their explicit `eye_position` dependency.
- The ordinary indexed dynamic-shader route was removed. Full per-draw bounds
  classification during the test found no lighter-adjacent candidate; every
  observed draw was ambient geometry 17-29 units from the native lighter. It
  was a differential false positive caused by recycled dynamic-buffer offsets.
- Sustained/proximity artifact:
  `Builds/lighter-sustained-particle-proximity-20260823/d3d11.dll`, SHA-256
  `B17FCB5C31771981C1F5D77ADC50A76834554B10F27DFEC72700A41F2AD78128`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Billboard-disable failure and coordinate-space eye correction

- Test of `B17F...` proved the sustained proximity classifier was active, but
  the flame looked abnormal and still moved with the HMD; the visible secondary
  flame/smoke also remained HMD-attached. The deployed DLL was immediately
  restored to normal-looking artifact `02139...` before the next build.
- The active-classification log rules out selection failure: ignition and
  sustained draws repeatedly classified `LIGHTER` at distances `0.099-0.255`
  from the native lighter. Disabling `particles_flags.z` itself was wrong;
  that branch is necessary for Metro's intended billboard shape.
- Root coordinate mismatch: exact VS `E1FDE311D1E35018` generates billboard
  axes from VS b4 `eye_position` before multiplying by `m_WVP`. Metro supplies
  that eye in world space. Once cb0 is reparented into the completed lighter's
  view-space hierarchy, leaving b4 in world space makes the particle generator
  combine two coordinate systems, retaining HMD-dependent pivot motion.
- The new build restores the unchanged native `cb_particles` and the last
  normal-looking rigid flame matrix path. For each eye, it inverts the final
  corrected WV and supplies the selected eye origin expressed in particle-local
  space through a private 128-byte VS b4. Thus Metro's billboard and animation
  branches remain enabled, but their eye vector is consistent with the new
  lighter-local transform.
- Billboard-eye-local artifact:
  `Builds/lighter-billboard-eye-local-20260823/d3d11.dll`, SHA-256
  `AE84A6325F729242D2911CE553214C9EB12DD47DA2883B315C78BD3B479E905A`;
  Release x64 succeeded and the same hash was deployed to Metro.

## Eye-local failure, oversized capture, and post-VS pivot approach

- Test of `AE84...` restored the normal flame shape but did not remove HMD
  motion; secondary flame/smoke also remained HMD-attached. This disproves b4
  `eye_position` as the positional cause. Deployment was restored to stable
  normal-looking artifact `02139...`.
- Automatic paired frame capture initially created empty folders because
  `hunting=0` selects `HackerContext`, which has no resource-dump support.
  Temporarily setting `hunting=1` selected `FrameAnalysisContext`, but full
  persistent `DUMP_RT|DUMP_CB|DUMP_VB|DUMP_IB` at the 3968x2203 render size was
  far too expensive: Metro stopped responding and was force-closed.
  `hunting=0` was restored immediately and automatic lighter capture was
  disabled in source. Do not repeat that full capture mode.
- The partial capture at
  `FrameAnalysis-Lighter-01-20260823-235017-055` contained 104,716 files and
  occupied approximately 59.9 GB. After the user explicitly requested its
  removal, the exact capture directory was verified under the Metro game root
  and permanently deleted on 2026-08-24. `hunting=0` remains restored and the
  automatic capture is disabled; do not recreate this full capture.
- Runtime normalized on/off census in the same scene produced only three
  lighter-exclusive stable render signatures:
  - solid body: VS `79BA9F1ECFAF0CA1`, PS `6597099FD3B895DB`, 2499 indices;
  - particle material 0: VS `E1FDE311D1E35018`, PS
    `C7170B0AB238E3EC`, 6 indices;
  - particle material 1: the same VS, PS `252FCE3C8790BFBC`, 6 indices.
  Therefore the visible main flame, secondary flame and smoke are layers of
  this one native particle system, not the earlier dynamic indexed shader.
- Matrix/constant rewriting deforms this system because its VS fully generates
  animated, camera-facing vertices from coupled instance and particle inputs.
  New approach: leave Metro's VS, cb0, b4, b10, instance buffer, textures and PS
  completely native. A pass-through geometry shader receives the completed
  triangles and adds a per-eye NDC translation from their current native pivot
  to a stable material-specific pivot attached to the completed lighter.
- Each PS material captures its source `inst0.xyz` once. That point is converted
  from native particle/world space to centre-eye space, then through
  `inverse(nativeLighter)` to a persistent lighter-local anchor. Every frame and
  eye, native and desired anchor points are projected; their NDC difference is
  supplied to the GS. This preserves native billboard shape, smoke direction,
  atlas animation, colour and gameplay behavior while moving the finished
  visual layers with the wick.
- Post-VS-pivot artifact:
  `Builds/lighter-post-vs-pivot-20260824/d3d11.dll`, SHA-256
  `FA1326546C6986640F22F8BBBDE3B92A92E3D7827FBEB270B3A137DF4BE4AFF7`;
  Release x64 succeeded, the same hash was deployed, `hunting=0`, and automatic
  full capture is disabled.

## Post-VS pivot coordinate-space correction

- Headset test of `FA132...` showed no attachment improvement; the native
  flame only appeared slightly smaller. This proves the GS route executed but
  that its target/depth calculation was wrong. Treat `FA132...` as a failed
  diagnostic, not a usable attachment build.
- Root cause in the pivot math: `sFinalLighterInstance` is deliberately stored
  in Metro's original viewmodel space after pre-cancelling
  `vrViewCorrection3x4`, because the ordinary solid-lighter projection applies
  that correction later. The pivot code projected this stored matrix directly
  as if it were already in final VR view space. Its initial anchor capture also
  compared a particle point after the VR view transform against the uncorrected
  native lighter instance. Both comparisons mixed coordinate spaces.
- Anchor capture now evaluates the source particle in eye-0 VR view, applies
  `inverse(vrViewCorrection3x4)` to return it to Metro viewmodel space, and only
  then applies `inverse(nativeLighter)`. Each desired per-eye point is built
  from the corrected lighter-local anchor and passed through that eye's view
  correction before projection, matching the solid lighter's real render path.
- The GS now changes only NDC X/Y. NDC Z is left native so the diagnostic cannot
  shrink the flame, alter its perceived depth, or change particle depth tests.
- Corrected-view-space artifact:
  `Builds/lighter-corrected-view-space-pivot-20260824/d3d11.dll`, SHA-256
  `B3DDE5C2949E396BA377443EAA7198DD7EDCAF45A5D25B9C30D9E0FCD80424FD`;
  Release x64 succeeded and the same hash was deployed to Metro. `hunting=0`;
  no automatic capture or high-volume diagnostic is enabled.

## Main flame attached; secondary dynamic effect correction

- Headset test of `B3DDE...` is the first successful flame attachment: the
  primary flame no longer moves with the HMD and moves only with the lighter.
  Its apparent size is still smaller than the original and must be corrected
  after all layers share the right parent. The secondary flame/smoke remained
  HMD-relative.
- Live draw data disproves the multiple-instance theory: both six-index native
  billboard materials submit exactly one instance. The missing secondary
  pieces correspond to the separate 12-index `DrawIndexed` first observed
  after the lighter had burned for a while: VS `77840E246BBF6E55`, PS
  `8281F4D8AE4065B0`. This shader is shared with ambient effects, so shader
  identity alone remains unsafe.
- The earlier dynamic bounds classifier had the same space error as the failed
  main-flame pivot. It transformed the vertex centre into corrected VR view
  space but compared it to `sNativeLighterInstance` in Metro's uncorrected
  viewmodel space, leading to the false conclusion that all candidates were
  17-29 units from the lighter. The classifier now transforms the native
  lighter through `vrViewCorrection3x4` and compares both points in eye-0 VR
  view space. It additionally requires the exact 12-index shader pair and the
  offscreen effect pass.
- Accepted secondary geometry retains its complete native vertex data and
  animation. Its native eye-0 WV is transformed by `inverse(C)` into Metro
  space, reparented through `sLighterParentDelta`, then transformed by each
  selected eye's `C`. This is the same old-space -> completed-parent -> VR-eye
  ordering used by the now-working primary flame correction.
- Secondary-effect artifact:
  `Builds/lighter-secondary-effect-corrected-parent-20260824/d3d11.dll`,
  SHA-256
  `A3B4A7861C988015DC1DD49ADC4B7EF25182271795F962C05409AFAB3B3A217F`;
  Release x64 succeeded and the same hash was deployed to Metro. `hunting=0`;
  automatic capture remains disabled.

## Secondary route disproved; bounded call-neighbourhood trace

- Headset test of `A3B4...` left the secondary flame/smoke attached to the HMD.
  Environmental flames in another level were unaffected, confirming that the
  safety gate did not broadly modify the shared effect shader.
- The log shows why there was no visible change: every tested 12-index
  `77840E.../8281F...` draw remained approximately 16 game units from the live
  lighter after both positions were corrected into the same eye-0 view space.
  This is conclusively environmental geometry, not the missing lighter layer.
  The experimental reparent route was removed; the spatial classifier remains
  available only as diagnostic evidence.
- Added a bounded text-only call-neighbourhood trace around the proven sustained
  six-index lighter particle. It records the preceding/following 24 draw calls
  on its first occurrence and again on occurrence 180 (roughly three seconds
  later), including draw type/count, shader hashes, layout class, viewmodel and
  effect classification, viewport and topology. It writes under 100 short log
  lines, dumps no GPU resources, and cannot create another large capture.
- Neighbour-trace artifact:
  `Builds/lighter-secondary-neighbour-trace-20260824/d3d11.dll`, SHA-256
  `6E883FBB90778C955DF104DED3020F477F52A31F60CD9E9A87B863855421E63D`;
  Release x64 succeeded and the same hash was deployed to Metro. `hunting=0`;
  automatic capture remains disabled; primary-flame attachment is unchanged.

## Growing secondary particle batches identified and reparented on GPU

- The bounded trace produced both complete windows successfully. It proves the
  earlier one-instance conclusion applied only to the first differential
  occurrence, not the sustained particle system:
  - the already-correct sustained core, PS `252FCE3C8790BFBC`, remains exactly
    one instance;
  - PS `FB0FCA1B2C641F6E` grows from 1-3 instances at ignition to 35 instances;
  - PS `C7170B0AB238E3EC` grows from 2 instances to 14 instances.
  All three use VS `E1FDE311D1E35018`, the 64-byte non-matrix particle layout,
  six indices and the viewmodel pass. The C717/FB0F batches are the missing
  secondary flame/smoke layers.
- A single NDC pivot per draw can correctly move the one-instance 252F core but
  cannot describe dozens of independently positioned particles. Splitting the
  batch and CPU-reading every instance each frame would introduce many GPU
  stalls, so the new path remains entirely GPU-side.
- Added a second pass-through geometry shader for C717/FB0F. Metro first fully
  generates each native animated billboard vertex. The GS then applies the
  homogeneous mapping
  `P_eye * C_eye * lighterParentDelta * inverse(C_eye) * inverse(P_eye)`
  to every completed clip-space vertex. Thus every instance receives the exact
  native-lighter -> completed-left-hand parent while preserving its native
  instance payload, UVs, colour, animation and shape. No vertex/constant data
  is read back or reconstructed.
- The proven one-instance 252F core deliberately retains the working
  translation-only pivot to avoid regressing the first successful attachment.
  Both routes explicitly redraw each eye and restore the original GS/b0 state.
- All-particle-batches artifact:
  `Builds/lighter-all-particle-batches-reparent-20260824/d3d11.dll`, SHA-256
  `55C1E1998F35AEB05D2F7AEF2F7AEA40811B08427289DC8C75276785C3F351DB`;
  Release x64 succeeded and the same hash was deployed to Metro. `hunting=0`;
  automatic frame capture remains disabled.

## All layers attached; unified flame calibration

- Headset test of `55C1...` confirms every primary/secondary flame and smoke
  layer now follows the left controller. Remaining work is purely local
  placement: restore the native flame's larger size, align the secondary cloud
  to the primary, then place the complete assembly over the wick.
- The user supplied two older reference screenshots for relative size only and
  explicitly said to ignore their other content. Their apparent flame-height
  ratio supports an initial uniform scale of approximately `1.5`; this is now
  the distributable default rather than retaining the visibly undersized 1.0.
- Added persistent `vr_lighter_flame_offset.txt` with seven values: whole-group
  lighter-local XYZ, secondary-only local XYZ, and uniform scale. Invalid or
  absent files fall back to zero offsets and scale `1.5`; positions clamp to
  +/-0.5 and scale to 0.25-3.0.
- Toggle the independent flame calibration by holding LT + both grips for about
  one second while the lighter is visible. This exact chord excludes lighter
  body and weapon calibration. In the mode:
  - left stick X/Y moves the complete flame assembly laterally/vertically;
  - left grip + left-stick Y moves the complete assembly in depth;
  - unmodified right-stick Y changes uniform size;
  - unmodified right-stick X moves only the secondary layers on local X;
  - right grip + right-stick Y/X moves only the secondary layers on local Y/Z.
  Hold LT + both grips again to save and leave the mode. Movement/turning is
  suppressed while calibration owns the sticks.
- The one-particle core GS now scales its finished screen-space geometry around
  its native pivot and targets `primaryAnchor + wholeGroupOffset`. The batch GS
  constructs `finalLighter * localAdjustment * inverse(nativeLighter)`, where
  local adjustment scales around the captured primary lighter-local anchor and
  adds whole-group plus secondary-only offsets. This keeps every adjustment
  inside the solved controller hierarchy and cannot reintroduce HMD motion.
- Unified-flame-calibration artifact:
  `Builds/lighter-flame-unified-calibration-20260824/d3d11.dll`, SHA-256
  `76B7255A46756E329993D286AA493460A1082E7CBA7A705951D71863C1ACB220`;
  Release x64 succeeded and the same hash was deployed to Metro. `hunting=0`;
  automatic capture remains disabled.

## Rigid secondary-batch swing removed

- Video `2026-08-24 12-54-18.mp4` (47.315 seconds, 2839 frames at 60 fps)
  shows the enlarged flame/glow moving around during locomotion and rotating
  around its bottom during both physical and stick turning. The user described
  this accurately as a bottom-pivot rotation rather than renewed HMD drag.
- Saved calibration from that run is preserved:
  - group `(-0.000229, 0.007892, 0.011867)`;
  - secondary `(-0.005678, 0.020088, 0.030312)`;
  - scale `2.117610`.
- Root cause: the C717/FB0F batches were transformed by a full post-VS rigid
  `finalLighter * inverse(nativeLighter)` mapping. This attached their positions
  but also rotated Metro's already-generated camera-facing/world-up billboard
  geometry with the controller. At scale 2.118, that incorrect rotation was
  especially visible as the glow/smoke swinging around the wick.
- The growing batches now use the same translation-only NDC pivot as the proven
  one-particle 252F core. Once the core's layer-1 anchor exists, C717/FB0F share
  its native source and desired wick pivot; layer 0 is a brief ignition fallback.
  Their saved secondary-only XYZ is added in lighter-local space before target
  projection, and the common scale is applied around the shared native pivot.
  Metro therefore retains its native flame orientation/animation while every
  layer follows one wick translation. No full rigid rotation is applied.
- Shared-wick-pivot artifact:
  `Builds/lighter-batches-shared-wick-pivot-20260824/d3d11.dll`, SHA-256
  `5CDA83C30AED7B45457D491A3329B752480AC5A8B2D7C6051FDB7A82648B35B1`;
  Release x64 succeeded and the same hash was deployed to Metro. Existing
  `vr_lighter_flame_offset.txt` was preserved, `hunting=0`, and automatic
  capture remains disabled.

## Native particle-depth dependency removed

- Follow-up video `Video Project 56.mp4` (52.967 seconds, 1589 frames at
  30 fps) shows that the shared NDC pivot did not remove the movement. It also
  makes a second symptom clear: the visible flame grows and shrinks during
  turning. The wick remains attached, but the particle cloud changes size and
  its upper extent moves around the fixed bottom.
- The runtime pivot log provides a direct numerical cause. The completed wick
  stays near view-space Z `0.35-0.38`, while Metro's original particle emitter
  varies through approximately Z `0.46-0.70` during ordinary movement and can
  jump far away during a transition. The NDC translation deliberately retained
  that native depth, so the native emitter continued controlling the projected
  size and relative shape even after its screen position was moved to the wick.
- Replaced the post-VS NDC shift with a per-eye view-space transplant. The GS
  now deprojects each completed billboard vertex with the current eye's inverse
  projection, measures its view-space offset from the current native particle
  centre, places that offset around the controller-relative wick centre, and
  reprojects with the same eye projection. This gives every vertex the wick's
  real depth without applying controller rotation to Metro's camera-facing
  billboard geometry.
- The existing user scale `2.117610` was calibrated in NDC. On the first stable
  left-eye sample, the code records one fixed native-depth-to-wick-depth scale
  conversion and combines it with that saved value. The conversion is never
  recomputed from the wandering native emitter, preserving the user's apparent
  size while preventing the old grow/shrink feedback.
- Saved group position, secondary position and scale file remain unchanged.
  Automatic capture and high-volume diagnostics remain disabled.
- View-space-wick-depth artifact:
  `Builds/lighter-viewspace-wick-depth-20260824/d3d11.dll`, SHA-256
  `BA138C07DCA38544392E926C7ED29E8BA677207DB4A16C1EA9DFC29B9B8F3B83`;
  Release x64 and an independent `fxc` compile of the runtime geometry shader
  both succeeded. The same DLL hash was deployed to Metro, the seven saved
  calibration values were verified unchanged, and `hunting=0`.

## Objective-directed flame behavior retained; sustained base pinned

- The user recognized that the smoke intentionally points toward the current
  objective and that the main flame points in the same direction. The rotation
  itself is therefore native gameplay behavior, not a remaining HMD/controller
  transform error. The actual defect is that the flame rotates around its
  centre instead of keeping its authored base centred on the lighter wick.
- Disassembly of VS `E1FDE311D1E35018` confirms it constructs each particle
  quad after applying Metro's live `particles_T/R/U` orientation. Its output
  `TEXCOORD1.xy` contains the animated atlas UVs. The six-index quad is submitted
  as triangles `(0,1,2)` and `(3,2,1)`; both triangles describe the same affine
  UV plane even when one base corner is not directly present.
- For the one-quad sustained `252FCE3C8790BFBC` flame only, the view-space GS
  now solves the full quad's base midpoint from each triangle's three
  `TEXCOORD1.xy` values and deprojected positions. It uses the midpoint of the
  maximum-V atlas edge as the pivot and maps that pivot to the calibrated wick
  target. The two triangles independently reconstruct the same point, so the
  quad remains continuous. Metro remains free to rotate the opposite edge and
  therefore aim the flame toward the objective, while the base stays fixed.
- The growing C717/FB0F smoke batches deliberately retain the shared emitter
  view-space transplant. Pinning every independent smoke particle to the wick
  would collapse the native trail and remove the objective-direction feedback.
- The embedded runtime shader passed an independent `fxc` `gs_5_0` compile and
  the complete Release x64 build succeeded. Existing flame calibration remains
  unchanged; automatic captures and high-volume logging remain disabled.
- Objective-flame-base-pin artifact:
  `Builds/lighter-objective-flame-base-pin-20260824/d3d11.dll`, SHA-256
  `BD5F0F3331791919FD47A64562CB6CF9D3F8F2E80CA766553D428CE3402AC8B2`;
  the same hash was deployed to Metro. The seven saved calibration values were
  verified unchanged and `hunting=0`.

## Final lighter result confirmed and calibration embedded

- Headset testing confirms the completed behavior is correct: the lighter body
  stays on the left hand, the flame/smoke assembly follows the lighter, the
  objective-directed flame and smoke continue to aim normally, and the main
  flame's base remains centred on the wick. The user described the result as
  perfect after a final small calibration adjustment.
- Final lighter-body calibration from `vr_lighter_offset.txt`:
  - position `(0.111402, -0.105089, 0.682597)`;
  - rotation `(1.058952, -0.266553, 0.702228)` radians.
- Final post-base-pivot flame calibration from
  `vr_lighter_flame_offset.txt`:
  - whole group `(-0.000229, -0.011636, 0.013467)`;
  - secondary flame/smoke `(0.010559, 0.020088, -0.005062)`;
  - uniform scale `2.117610`.
- Both sets are now compiled into `VRPose.cpp` as the clean-install defaults.
  Missing calibration files therefore reproduce the headset-verified layout
  for every distributed copy. Valid local files remain optional overrides for
  users who deliberately recalibrate. Malformed files fall back to the same
  embedded values instead of the old zero/1.5 development baselines.
- Removed the completed lighter-identification calls from all four draw entry
  points. This eliminates the bounded startup particle readback and the
  per-draw differential/neighbour bookkeeping from release execution. The old
  helper implementations remain inert as reverse-engineering documentation;
  automatic lighter frame capture is still hard-disabled.
- Final embedded release artifact:
  `Builds/lighter-left-hand-final-embedded-20260824/d3d11.dll`, SHA-256
  `E28517205A63DC3734AC735DBC47B38DDDEF2F2CF221653C4EA77EEA43AB2844`;
  Release x64 succeeded and the identical DLL was deployed to Metro with
  `hunting=0`. Both external calibration files were left intact and verified
  against the embedded defaults.

## Journal-presented lighter follow-up started

- Opening the journal also presents a visually identical lighter, but live
  testing shows that version remains on the right-hand/weapon parent and its
  flame/smoke remains under Metro's native HMD-relative particle path.
- The latest journal session contains objective draws from frames 5019-5117 but
  no `lighter bones` or `body reparented` entry. Therefore the journal lighter
  never reaches the exact regular-body signature; the missing body match also
  prevents `sFinalLighterInstance` from refreshing, so the already-solved
  particle route correctly declines to transform the associated effects.
- Added a one-session, text-only DrawIndexedInstanced differential. Before the
  first journal open it learns stable relevant draw identities; while open it
  logs only new identities; on close it disarms permanently. The stable key is
  index count, VS/PS hashes, layout class, viewmodel classification and
  topology. Live instance count/start/base/frame are included only as evidence.
  It performs no GPU buffer readback and cannot create a frame-analysis folder.
- Journal-lighter differential artifact:
  `Builds/journal-lighter-differential-20260824/d3d11.dll`, SHA-256
  `3C71AEDDD619C05E57BD9105F949051870FD70A5589BFA1F5C3F0B4C2EA2965F`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.

### Journal-lighter body and effects identified

- The user opened the journal, immediately opened its lighter lid, left the
  journal visible, then closed it. This was the correct sequence: the trace saw
  the closed body beginning at frame 1879 and the ignited effects beginning at
  frames 2042/2048 before disarming normally on the journal-close edge.
- The journal lighter body is the same 2,499-index skinned model and the same VS
  `79BA9F1ECFAF0CA1` as the standalone lighter. Its only stable render-identity
  difference is material PS `4C64256218CECB8B` instead of standalone PS
  `6597099FD3B895DB`. The journal differential also found several journal-only
  meshes, but none share this exact index-count/VS pair.
- Ignition submitted the already-solved viewmodel particle pair: C717 at frame
  2042 and the sustained 252F core at frame 2048. Therefore the body signature
  is the only missing parent link; once it updates `sFinalLighterInstance`, the
  existing smoke batches, view-space depth correction, objective direction and
  wick-base pin apply without a second particle implementation.
- Added PS `4C64256218CECB8B` as an alternate material on the exact 2,499-index
  lighter classifier. Both variants now share live lighter bones/lid animation,
  visible-centre solve, completed-left-hand parent and embedded calibration.
  Removed the diagnostic call from DrawIndexedInstanced; its helper is inert.
- Journal-lighter left-hand-parent artifact:
  `Builds/journal-lighter-left-hand-parent-20260824/d3d11.dll`, SHA-256
  `708213B61832E29018F15C1B79445458950C4565C5879AF8388E49AB46F3A6F9`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  Existing lighter-body and flame calibration files were preserved unchanged.

### Journal-lighter attachment confirmed; independent calibration added

- Headset testing confirms the journal-presented lighter now uses the completed
  left-hand parent correctly. Its live lid animation works, the primary flame
  and secondary flame/smoke follow it without HMD drag, and the existing
  objective-directed wick-base behavior carries over. Only placement differs
  from the standalone lighter and requires calibration.
- The material distinction is now retained as a runtime variant flag. Body PS
  `4C64256218CECB8B` marks the journal lighter; standalone PS
  `6597099FD3B895DB` marks the regular lighter. The selected flag is propagated
  from the exact body draw through `sFinalLighterIsJournal` to both particle
  calibration lookups, so body and effects always select the same profile.
- Added independent journal-only calibration storage:
  - `vr_journal_lighter_offset.txt` for body position and rotation;
  - `vr_journal_lighter_flame_offset.txt` for whole flame position, secondary
    flame/smoke position and uniform scale.
- Journal defaults initially clone the embedded, headset-verified standalone
  values. The original `vr_lighter_offset.txt` and
  `vr_lighter_flame_offset.txt` remain completely separate and are neither
  loaded nor saved while journal-lighter calibration is active.
- Calibration automatically locks to the variant visible when the mode is
  entered. LT + right grip toggles body calibration; LT + both grips toggles
  flame calibration. While the journal lighter is present, its exact renderer
  identity takes precedence over the old journal-page calibration chord.
- Independent-calibration artifact:
  `Builds/journal-lighter-independent-calibration-20260824/d3d11.dll`, SHA-256
  `564F53EB1A3F8C73FC23C94513E6E68DE6CA79DE58003F69B06675AA61DE3142`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  The two completed standalone lighter files were preserved; the journal files
  are intentionally absent until the user finishes and saves each calibration.

### Journal lighter opening tilt isolated from lid animation

- After calibrating the journal lighter while closed, the user reported that
  Metro's authored opening animation tilts the complete lighter away from that
  placement. The desired behavior is a stationary calibrated body with only
  the lid/striker and flame effects animating.
- The private journal-only bone palette now captures bone 0's affine root on
  the first body frame of each journal presentation. On later frames it builds
  `closedRoot * inverse(liveRoot)` and applies that correction to the three
  lighter bone matrices at palette base rows 0, 3 and 6. This makes bone 0
  exactly equal the captured closed root while preserving every child bone's
  live transform relative to that root; the lid can therefore open normally
  without carrying the whole lighter through the authored presentation tilt.
- The correction runs only for material PS `4C64256218CECB8B`. The standalone
  lighter, other skinned models, journal calibration values and flame/smoke
  objective behavior are unchanged.
- Closed-root-lock artifact:
  `Builds/journal-lighter-closed-root-lock-20260824/d3d11.dll`, SHA-256
  `A943587A563729208DC012B890DC6D7096F16034DBF3925D6D134341FD242164`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  The saved journal body file (SHA-256 `B1AD1809...649AD`) and both completed
  standalone calibration files were verified unchanged after deployment.

### Final journal-lighter calibration confirmed and embedded

- Headset testing confirmed the closed-root lock works: the lighter remains in
  its calibrated hand placement while Metro's lid animation opens normally.
  The user then completed a final small body rotation adjustment and calibrated
  the entire primary flame plus secondary flame/smoke assembly.
- Final `vr_journal_lighter_offset.txt` values (SHA-256
  `AF47CDB607899BA542822796620E81E72E1C9A4BBE116FDD3052923EA351F0B5`):
  - position `(0.118595, -0.094402, 0.686963)`;
  - rotation `(1.167941, -0.089573, 1.254131)` radians.
- Final `vr_journal_lighter_flame_offset.txt` values (SHA-256
  `776FA5B6A9B4673AFF39692E8A11CA2810AE0D3FB958CCF533017375FE4D3FE5`):
  - whole group `(-0.190863, -0.500000, -0.256484)`;
  - secondary flame/smoke `(0.009221, -0.001862, 0.000256)`;
  - uniform scale `1.681419`.
- Both profiles are embedded as the missing/invalid-file clean-install
  defaults. The journal-only setup chords are disabled for release so normal
  input cannot accidentally change them. The loaders and independent files
  remain supported for deliberate future development. Standalone lighter
  values and the restored weapon-calibration control are unchanged.
- Final embedded artifact:
  `Builds/journal-lighter-final-embedded-20260824/d3d11.dll`, SHA-256
  `5FBA2C2F044854B614E8C6BA87FE1B540A228395FC5DDDE1FDF7580C9A9A6B39`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  Both journal files and both standalone lighter files were hash-verified
  unchanged after deployment.

### Fresh-restart regression traced to first-frame root capture

- The first test after deploying the embedded build placed the journal lighter
  outside the hand even though both deployed journal calibration files remained
  byte-for-byte identical to the values that had just worked. Embedding and
  control removal therefore did not alter the transform inputs.
- The remaining session-dependent input was the closed-root lock: it captured
  the first body draw of each journal presentation. That draw can belong to
  Metro's journal presentation/draw-in animation, so different starts could
  freeze different roots and make the same calibration produce a different
  visible placement.
- Root acquisition now begins with an unlocked candidate. It requires eight
  consecutive body frames whose complete root affine changes by no more than
  `0.0005` before committing the closed reference. Movement during the draw-in
  animation resets the candidate instead of becoming the permanent root.
  Until the stable reference is committed, the live palette is left unchanged.
- Journal-only calibration is temporarily re-enabled for this verification
  build in case the deterministic stable reference needs one final adjustment.
- Stable-root-capture artifact:
  `Builds/journal-lighter-stable-root-capture-20260824/d3d11.dll`, SHA-256
  `352CED7BFDE831D403F671293864F90D551BD326B5C5027796308CB93D75573F`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  Both journal calibration files retained their final pre-test hashes.

### Cross-variant pivot contamination and journal scale ceiling diagnosed

- The next test exposed two separate issues. The journal flame stopped growing,
  and after journal body calibration the standalone lighter appeared outside
  the hand.
- The files and runtime log give definitive causes:
  - journal flame calibration reached the hard-coded `3.000000` ceiling;
  - journal body/flame modes were correctly selected and only the two journal
    files changed. Both standalone files retained their completed hashes and
    original values, so journal calibration did not overwrite them.
- The standalone displacement came from learned renderer state rather than its
  saved transform. The 2,499-index body scanner kept one global vertex/bone
  centre coefficient set and scanned only whichever lighter appeared first.
  In this run the journal lighter supplied that set. When the standalone model
  later appeared with its different live VB/base range, it reused the journal
  centre and solved the wrong visible pivot; the log shows the evaluated centre
  changing from the journal range near `(-0.181,-0.159,0.828)` to the regular
  range near `(0.054,0.045,-0.019)` without either saved file changing.
- Centre coefficients and scan-complete state are now independent for variant
  0 (standalone PS `6597...`) and variant 1 (journal PS `4C64...`). The native
  particle source centres, lighter-local anchors, fixed view-scale conversions
  and legacy rigid-emitter locals are also separated by variant so appearance
  order cannot contaminate body or flame placement in a later session.
- Only the journal flame scale ceiling is raised from `3.0` to `6.0`; the
  standalone lighter retains its existing limit and values.
- Variant-isolation artifact:
  `Builds/lighter-variant-isolated-state-20260824/d3d11.dll`, SHA-256
  `273F50994E9E29A26E54E1A5FD9931CDC46FE2F41E185EE6CFD1B0C3A78193DA`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  All four calibration files were hash-verified unchanged after deployment.

### Both lighter variants independently verified and final values embedded

- The user recalibrated the journal lighter, then verified its changes did not
  affect the standalone lighter. The standalone body returned to its correct
  hand placement once per-variant centre scanning was active; only its particle
  assembly needed a small update for the newly independent regular anchor. The
  user calibrated that assembly and verified neither variant now affects the
  other.
- Final journal body file (SHA-256
  `59097187A07DBE9DBA80F6EA60306E7CA213AA528CF9128FEFE77193D89650B3`):
  - position `(0.118595, -0.094402, 0.686963)`;
  - rotation `(1.301412, -0.319485, 2.055675)` radians.
- Final journal particle file (SHA-256
  `16D8F954ADAFF3BBB5A699FB8E584531C476021E9103A2DA59EB626E98225F9C`):
  - whole group `(-0.100830, -0.133042, 0.094347)`;
  - secondary flame/smoke `(-0.003379, 0.002066, 0.002649)`;
  - scale `1.737870`.
- Final standalone particle file (SHA-256
  `EBE5B912B7408A35D0F11B08D892FCF2DD56A6E24AFA6E6F6E1C91993430EE37`):
  - whole group `(0.009440, -0.013285, 0.007984)`;
  - secondary flame/smoke `(0.006361, 0.001222, -0.000862)`;
  - scale `1.984133`.
- These values replace the prior embedded defaults. The standalone body keeps
  its already-final `(0.111402,-0.105089,0.682597)` position and
  `(1.058952,-0.266553,0.702228)` rotation. Both lighter body/flame calibration
  chords are disabled for release; weapon calibration remains enabled.
- Final isolated/embedded artifact:
  `Builds/lighter-variants-final-embedded-20260824/d3d11.dll`, SHA-256
  `2DD4754E2ED5980D423C63F02A689B56314A0C3B0E658FA8F357B7481826ADE4`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  All four body/particle calibration files matched their expected final hashes
  after deployment.

### Regular flame restart drift traced to first-particle depth learning

- Fresh-restart testing left the journal lighter correct and the standalone
  body correct, but moved the standalone flame/smoke slightly despite the
  standalone particle file retaining its exact verified hash.
- The current runtime log proves the remaining session input. At first ignition
  the standalone path captured sustained source `(0.00062,0.01648,-0.00031)`,
  local anchor `(-0.0015,0.1389,-0.0127)`, and a view-scale conversion of
  `0.5427` from `desiredZ/nativeZ = 0.3616/0.6663`. The journal independently
  captured `(0.00012,0.01700,-0.00056)`, `(-0.0551,-0.0056,0.7306)`, and
  `0.6415`. The conversion depended on controller/native-emitter depth at the
  first particle, so it could change the effective scale and placement after
  every process restart while the text calibration file stayed unchanged.
- Sustained layer 1 now starts with those verified source centres, local anchors
  and view conversions embedded separately for standalone and journal. Its
  validity flags start true, eliminating runtime first-particle learning for
  the persistent flame and shared secondary smoke pivot. Brief ignition-only
  layer 0 may still learn transient state because it does not control the
  sustained assembly.
- Both body calibrations and journal flame calibration remain disabled. Only
  standalone flame calibration is temporarily re-enabled for one final
  adjustment against the new deterministic reference.
- Deterministic-particle-anchor artifact:
  `Builds/lighter-deterministic-particle-anchors-20260824/d3d11.dll`, SHA-256
  `E8BE0E8659675B0195C530DCCE66A81ADAAED338A22B12A08FA97673F46DECFB`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  All four calibration files retained their pre-build hashes.

### Deterministic regular flame calibrated and restart-verified

- The user calibrated the standalone flame/smoke against the fixed per-variant
  anchor, restarted the game, and confirmed the placement persisted. This is
  the required proof that no first-particle/controller-depth input remains.
- Final standalone particle file (SHA-256
  `181EA418865073E2FA99FA0EFD447C8F2D800C4C487A784DBAA852548A5FA453`):
  - whole group `(0.000966, -0.009818, 0.013574)`;
  - secondary flame/smoke `(0.010561, 0.001222, -0.000862)`;
  - scale `1.878490`.
- These values replace the previous standalone particle defaults. The final
  temporary standalone flame chord is disabled; journal flame and both body
  calibration modes remain disabled. Weapon calibration remains enabled.
- Completed lighter artifact:
  `Builds/lighter-complete-final-20260824/d3d11.dll`, SHA-256
  `5C43D5A33469F19685B872838A091443456CBBD1BCBEC4F8E9F8504D87EF9171`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  All four final calibration files matched their expected hashes after
  deployment.

## Prologue hand split regression follow-up

- After the completed lighter work, the user loaded the prologue and found its
  hands combined again. Review proves the lighter commit changed no prologue
  classifier/split lines, and both prologue hand/watch calibration files retain
  their August 23 values. The current runtime log still contains a 21,252-index
  draw but lacks every `VRPose prologue: split opening mesh` line, proving the
  existing split path is present but its exact classifier returns false.
- The classifier requires not only the unique index count and known VS/PS but
  hard-coded pooled locations `start=966255`, `base=223462`. Those are the most
  likely unstable fields.
- Added a bounded text-only probe inside `IsOpeningSequenceArmMesh`. For up to
  16 unique 21,252-index/one-instance candidates it records start/base, VS hash,
  PS hash and whether the old exact predicate passed. It performs no resource
  readback, enables no hunting/capture, and logs each identity only once.
- Classifier-diagnostic artifact:
  `Builds/prologue-arm-classifier-diagnostic-20260824/d3d11.dll`, SHA-256
  `0923EF97F22368EA0659A8A49ACA5C33EC1F59AE6FBB2326A9ED8287AD6311BB`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  Both prologue calibration files were hash-verified unchanged.

### Chapter-start versus checkpoint prologue identities confirmed

- The diagnostic captured both variants in one run:
  - chapter-start/scripted-event candidate: `start=2337777`, `base=612307`;
  - later checkpoint-loaded working candidate: `start=966255`, `base=223462`.
- Both are otherwise identical: 21,252 indices, one instance, VS
  `79BA9F1ECFAF0CA1`, PS `32B3ADE9DE302235`. This exactly explains why starting
  the chapter showed combined hands while loading back into its checkpoint
  restored separation. Nothing in the lighter work changed the prologue path;
  the old classifier mistook pooled geometry locations for stable identity.
- Removed the diagnostic and removed start/base from the predicate. The split
  now requires the unique index count, instance count and both established
  shader hashes, recognizing both scripted and checkpoint submissions while
  retaining a substantially narrower identity than index count alone.
- Pool-independent-classifier artifact:
  `Builds/prologue-arm-pool-independent-classifier-20260824/d3d11.dll`, SHA-256
  `EE845242E8FBBE133F30A1F8BF27CB217B9A47261645C725B9924BF88D1E002D`;
  Release x64 succeeded and the identical DLL was deployed with `hunting=0`.
  Both prologue calibration files were hash-verified unchanged.

- The user restarted the prologue chapter with this build and confirmed the
  hands now separate correctly at chapter start. This closes the regression;
  no diagnostic remains enabled.

## Prologue lighter profiles isolated from normal gameplay

- The prologue contains both the standalone equipment lighter and the lighter
  presented alongside the journal. The user confirmed both already inherit the
  separated left-hand path and that body, lid, primary flame, secondary flame
  and objective smoke remain correctly assembled. Metro reuses the same exact
  2,499-index body meshes/materials used later in the game, so mesh identity
  alone cannot distinguish which hand skeleton owns the lighter.
- The completed left-hand publication now records whether its parent came from
  the normal or prologue skeleton. At the exact lighter-body draw, that flag is
  combined with the existing material distinction into a four-way identity:
  - variant 0: normal-game standalone lighter;
  - variant 1: normal-game journal lighter;
  - variant 2: prologue standalone lighter;
  - variant 3: prologue journal lighter.
- That four-way identity is propagated through the entire assembly, not only
  the solid body: visible-centre coefficient scans, final rigid parent,
  sustained/ignition particle source centres, lighter-local particle anchors,
  fixed view-scale conversion, legacy rigid-emitter local transforms, body
  calibration, and whole/secondary flame calibration all select the same
  profile. Appearance order therefore cannot transfer learned prologue state
  into either completed normal-game lighter.
- The two prologue profiles are seeded from their matching verified normal-game
  values so this first build preserves the currently correct body-to-flame
  assembly. They save only to four new files:
  - `vr_prologue_lighter_offset.txt`;
  - `vr_prologue_lighter_flame_offset.txt`;
  - `vr_prologue_journal_lighter_offset.txt`;
  - `vr_prologue_journal_lighter_flame_offset.txt`.
  The existing `vr_lighter_*` and `vr_journal_lighter_*` files are never written
  by either prologue calibration mode.
- Only prologue lighter calibration is enabled. Main-game regular and journal
  body/flame profiles remain release-locked, and weapon calibration remains
  enabled. Controls retain the established layout:
  - body: hold modifier + right grip for one second to toggle; left stick moves
    X/Y, left grip + left-stick Y moves depth, right stick rotates, and right
    grip + right-stick X rolls;
  - flame/smoke: hold modifier + both grips for one second to toggle; left stick
    moves the whole assembly, left grip + left-stick Y moves depth, right-stick
    Y scales, right-stick X adjusts secondary X, and right grip + right stick
    adjusts secondary Y/Z.
- Independent-profile artifact:
  `Builds/prologue-independent-lighter-profiles-20260824/d3d11.dll`, SHA-256
  `3C865617320176D99C814FE0744AEC6E4CAC570A6234AFC1AA7640EEDA99D65C`.
  Release x64 succeeded with 0 errors (only the solution's pre-existing
  warnings), the identical DLL was deployed to Metro, and `hunting=0`.

### Prologue journal lighter opening identity fixed

- The user calibrated both independent prologue bodies successfully, but the
  prologue journal lighter again moved/tilted when opened.
- The runtime log proved that the stable closed-root lock did initialize
  (`locked stable closed root at frame=8468`) and that body calibration was
  correctly editing/saving variant 3. The remaining distinction was Metro's
  prologue-only material transition: the closed body uses the established
  journal material, but opening can submit the same exact body through the
  standalone material. That temporarily reclassified it as variant 2, bypassed
  the journal root correction and selected the wrong prologue calibration.
- During the prologue only, the synthetic journal presentation state is now
  authoritative for the presentation bit. Once the journal is presented, the
  exact lighter body remains variant 3 across the material change, keeping the
  saved prologue-journal placement and closed-root correction active while the
  lid and flame animation play. Normal-game classification is unchanged.
- Added a bounded once-per-120-frame log only when this material transition is
  actually observed: `retained journal identity across open material change`.
- Open-lock artifact:
  `Builds/prologue-journal-lighter-open-lock-20260824/d3d11.dll`, SHA-256
  `BE57E2E5E755D47CF1237ED6029D52037497ACD1C3DE758CA49EDC0D3CDAD2B3`.
  Release x64 succeeded with 0 warnings and 0 errors, the identical DLL was
  deployed with `hunting=0`, and both saved prologue body files were preserved.

### Material-switch hypothesis disproved; moving-centre cause isolated

- The user supplied `2026-08-24 20-31-24.mp4`. Frame review shows the lighter
  visibly swinging left/up as the prologue journal opens. The following runtime
  log contained no `retained journal identity` line, proving the material never
  changed and the preceding hypothesis was wrong. The temporary presentation-
  state override and its bounded log have therefore been removed.
- A prologue-only, text-only diagnostic captured live matrices for the three
  bones used by the 2,499-index lighter after saving their stable closed pose.
  It changed no transforms and required no recalibration. Artifact:
  `Builds/prologue-journal-lighter-bone-diagnostic-20260824/d3d11.dll`,
  SHA-256
  `A16CB45554CBD5AA22839E4FBBAF487CBC784F79EE03E0C6EB80ED2382E8B324`.
- The clean second run gave decisive measurements. Before opening all three
  bones differed from closed by only about `0.001..0.006`. During opening,
  bones 0 and 6 continued together and settled at `d=0.67328`, while bone 3
  diverged independently and settled at `d=1.74088`. This proves the existing
  root correction correctly identifies the common whole-prop motion and leaves
  bone 3 free to animate the lid.
- The remaining swing came after that correction: body placement recomputed the
  weighted centre of the complete visible mesh every frame. Moving bone 3
  changes that centre, so the rigid parent translated to recenter the opened
  lid—even though bones 0/6 had already been stabilized.
- Variant 3 now measures and stores its weighted mesh centre at the same stable
  closed pose used by the root lock, then uses that fixed centre for rigid
  placement throughout opening. The rigid body therefore keeps its calibrated
  position while bone 3 can still open the lid and start the flame. This change
  is prologue-journal-only; prologue regular and both normal-game variants keep
  their completed behavior. The matrix diagnostic was removed.
- Closed-centre-lock artifact:
  `Builds/prologue-journal-closed-centre-lock-20260824/d3d11.dll`, SHA-256
  `2414648480454726BB289949C10877361E352C416CDB39735A1FA799171AE89F`.
  Release x64 succeeded with 0 warnings and 0 errors, the identical DLL was
  deployed with `hunting=0`, and the user's saved prologue calibrations remain
  intact.

### Closed-centre lock disproved; rigid-child parent corrected

- The user reported no visible change from the closed-centre candidate. The
  following log confirmed the centre lock initialized and remained constant at
  `(-0.1723,-0.1618,0.8302)`, so neither variant selection nor weighted-centre
  recentering is the remaining source. The closed-centre override was removed.
- Code-path review exposed a hierarchy error already solved for the watch. The
  separated visible hand's completed instance intentionally compensates its
  live animated wrist landmark so the skinned wrist stays on the controller.
  `BeginLeftHandBonePalette` explicitly warns that rigid children must not
  inherit that compensation and publishes `sLeftWatchCompletedInstance` as the
  stable controller/wrist sibling. The prologue journal lighter was still using
  `sLeftHandCompletedInstance`, so the scripted journal wrist animation moved
  its rigid parent after all lighter-local corrections had completed.
- Variant 3 now captures the exact closed relationship
  `inverse(stableParent) * completedHand` when the existing eight-frame stable
  closed-root test succeeds. Every later frame rebuilds its body parent as
  `currentStableParent * capturedClosedRelationship`. At capture this is
  mathematically identical to the user's calibrated hand parent, so it causes
  no initial jump and needs no recalibration; afterward it follows the current
  controller while excluding only Metro's animated wrist compensation. Lid
  bone and flame/smoke animation remain downstream and unchanged.
- This bridge is scoped only to prologue journal variant 3. Prologue regular
  and both normal-game lighter variants retain their established parents and
  saved behavior.
- Stable-rigid-parent artifact:
  `Builds/prologue-journal-stable-rigid-parent-20260824/d3d11.dll`, SHA-256
  `7971C17758681A103D82E67FA816B27DF6D31111EA182CD23E19A6CE9CDA5CAD`.
  Release x64 succeeded with 0 warnings and 0 errors; the identical DLL was
  deployed with `hunting=0` and all saved calibrations preserved.

### Prologue journal lighter intentionally follows the animated hand

- The stable-parent build proved the remaining motion did not originate in the
  lighter: the lighter stayed fixed, revealing that Metro intentionally moves
  the prologue left hand during the journal presentation. The user requested
  either freezing that hand or having the lighter follow it.
- Following the hand is the lower-risk and anatomically correct choice.
  Freezing the hand would also alter its watch and the authored journal pose.
  The variant-3 stable-parent bridge was therefore removed, returning only the
  prologue journal lighter to the exact completed visible-hand parent. Its
  independent calibration, root correction, lid animation, and complete
  flame/smoke assembly remain unchanged. Prologue regular and both normal-game
  variants are untouched.
- Follow-hand artifact:
  `Builds/prologue-journal-lighter-follows-hand-20260824/d3d11.dll`, SHA-256
  `04D1D3B0A229A7D353A2DF8A2B51F77494CE7D752292B75B88B6387997A4B5F4`.
  Release x64 succeeded with 0 warnings and 0 errors; the identical DLL was
  deployed with `hunting=0`. Existing calibration files were preserved.

### Prologue journal lighter parented to the final visible hand bone

- Testing showed that simply restoring `sLeftHandCompletedInstance` made the
  lighter repeat its original motion rather than follow the visible hand. The
  completed instance is only the controller-level stage. In the prologue,
  `BeginLeftHandBonePalette` deliberately retains Metro's live hand palette;
  the scripted journal pose is applied afterward by those bones. Therefore a
  rigid child of the instance alone cannot follow the final rendered hand.
- The renderer now publishes `sPrologueVisibleHandAnchor` each frame as the
  exact completed prologue hand instance multiplied by live wrist/palm bone 51.
  Bone 51 is the dominant contributor in the measured prologue wrist landmark
  and is part of the exact palette bound to both eye draws.
- When prologue journal variant 3 reaches its established stable closed pose,
  it captures `inverse(visibleHandAnchor) * completedHand`. Its later parent is
  rebuilt as `currentVisibleHandAnchor * capturedClosedLocal`. This equals the
  user's calibrated parent at capture with no jump, then inherits the exact
  downstream bone animation seen on the rendered hand. Lighter-local root/lid
  correction and all flame/smoke paths remain downstream of that parent.
- This animated hand-bone parent is scoped only to prologue journal variant 3;
  all other lighter profiles and the hand/watch transforms are unchanged.
- Visible-hand-bone-parent artifact:
  `Builds/prologue-journal-visible-hand-bone-parent-20260824/d3d11.dll`, SHA-256
  `108E989A2DC87519CC855665C16DA0FE7B03C308265E2440208CBCF2B3649691`.
  Release x64 succeeded with 0 warnings and 0 errors, the identical DLL was
  deployed with `hunting=0`, and existing calibration files were preserved.

### Bone-51 wrist anchor replaced by an aggregate visible-hand anchor

- The user's recording `<PRIVATE_VIDEO>/2026-08-24 21-01-16.mp4`
  disproved bone 51 as a complete hand parent. The lighter initially met the
  hand, but separated progressively during the scripted journal pose. Bone 51
  follows the wrist joint; Metro continues deforming/translating the visible
  clenched glove around that joint, so a rigid child of bone 51 does not follow
  the apparent hand as a whole.
- The prologue hand palette path now constructs a rigid aggregate anchor from
  the same live palette and `prologueCentreCoeffs` used to calculate the exact
  skinned full-hand centroid. Its translation is that exact centroid. Its 3x3
  orientation basis is the coefficient-weighted average of all contributing
  prologue hand bones, orthonormalized with Gram-Schmidt and a cross product so
  scale/shear from skinning cannot distort the lighter.
- Prologue journal variant 3 captures the existing calibrated closed
  relationship as `inverse(aggregateVisibleHandAnchor) * completedHand` after
  the established eight-frame stable-root check. Every later frame rebuilds
  the parent from the current aggregate anchor and that captured relationship.
  The capture is algebraically identical to the saved calibrated placement,
  so this should require no recalibration while following the visible glove's
  aggregate translation and rotation through the scripted pose.
- The change remains strictly scoped to prologue journal variant 3. Prologue
  regular, normal-game regular, and normal-game journal lighter profiles are
  untouched. Lighter root/lid correction and the entire flame/smoke assembly
  remain downstream of the corrected parent.
- Aggregate-hand-parent artifact:
  `Builds/prologue-journal-aggregate-hand-parent-20260824/d3d11.dll`, SHA-256
  `CC7E2288DED627CE78A67B86C71FE404BF3DD5B20228AB718B028D8FC03E79C1`.
  Release x64 succeeded with 0 warnings and 0 errors; the identical DLL was
  deployed with `hunting=0`, with all saved calibrations preserved.
- User verification: confirmed in game that the prologue journal lighter now
  stays perfectly attached to the moving visible hand when the lighter opens.
  The saved body and flame calibration remained correct. This aggregate
  visible-hand anchor is the accepted solution; do not restore the discarded
  completed-instance-only, stable-controller, closed-centre, or single-bone-51
  parent experiments.

## Left-hand attachment release finalized after cleanup regression

- All normal-game and prologue left-hand, watch/time/cyan, regular lighter, and
  journal lighter attachment work is complete and headset-verified. The detailed
  implementation and rejected approaches in the preceding sections are the
  authoritative handoff for a future separated/original-hands option.
- The first release-cleanup build (`B7CB4AFE...`) removed and rearranged the
  lighter calibration branches inside the shared controller-input routine. In
  headset testing this unexpectedly broke both rotational head tracking (the
  world followed the HMD) and right-thumbstick turning. Restoring the exact
  pre-cleanup DLL (`CC7E2288...`) immediately restored both, proving the
  regression came from that cleanup rather than game state.
- The accepted safe cleanup preserves the verified input/control-flow code
  unchanged. Lighter calibration is disabled only through the existing
  `kPrologueLighterCalibrationEnabled = false` release gate. Do not structurally
  remove or reorder those branches again without an isolated headset test of
  head tracking and right-stick turning.
- Final prologue regular lighter defaults are embedded as position
  `(-0.095664, -0.029627, 0.653397)` and rotation
  `(0.046834, -0.478044, -0.014435)` radians. Final prologue journal lighter
  defaults are position `(-0.148426, -0.144054, 0.822864)` and rotation
  `(-0.864028, -0.067861, 0.319679)` radians. The normal regular/journal and all
  flame/smoke values remain the previously embedded verified defaults.
- The six local lighter calibration files were moved out of the game folder to
  `Builds/left-hand-attachment-release-cleanup-20260824/retired-game-overrides/`.
  Clean installs and the current development install therefore use embedded
  values. The established weapon calibration remains enabled.
- Automatic lighter frame capture and candidate suppression remain disabled by
  their existing false release gates. The six lighter capture folders and 5,307
  lighter-specific deduplicated payload files were deleted, recovering about
  3.24 GB. Keep the dormant discovery scaffolding until it can be removed in
  small, independently tested changes; the broad cleanup proved unsafe.
- Future separated/original-hands option: gate the complete feature at its
  classifier/reparent entry points. The disabled path must bypass left-hand
  palette isolation and all watch/time/cyan and lighter/flame child reparenting,
  allowing Metro's original combined viewmodel to render unchanged. Hiding only
  the final hand draw would leave rigid children behind and is not sufficient.
- Accepted safe-cleanup DLL:
  `Builds/left-hand-safe-cleanup-candidate-20260824/d3d11.dll`, SHA-256
  `046022EC57D844705669177BA712E1D773B8C47256CAF6C0E8EB5E77E325CC83`.
  User verified that head tracking, world stability, right-thumbstick turning,
  and basic left-hand attachment all work correctly. Deployed with `hunting=0`.

## Survival/Spartan watch variant caveat — 2026-08-24

- Metro's Survival and Spartan modes use different watch presentations. The
  current custom 3D time/filter-timer decoder and placement were developed and
  verified against the Spartan watch.
- Starting a new Survival game, quitting, and then restoring Spartan save files
  left the Survival watch mode active. In that state, affected saves displayed
  Metro's native flat time/timer on the HMD and did not initialize the custom 3D
  watch digits. Starting another new game in Spartan mode before restoring the
  saves restored the expected custom watch behavior.
- This was reproduced with the exact archived pre-performance-audit DLL, proving
  it was not caused by the asynchronous watch readback or CPU bone-palette
  optimizations. Treat Survival-watch support and persistence across restored
  saves as a separate future compatibility task.

### Physical-turn shooting A/B — first performance checkpoint rollback

The broad impact-particle isolation did not change the headset symptom and was
reverted completely (`6da87b4`). Additional testing showed that existing holes
disappear after a physical turn followed by thumbstick turning, new holes no
longer render normally, and shot accuracy itself declines after the physical
turn. This rules out a purely cosmetic particle/decal explanation.

For the next one-checkpoint A/B, the complete initial performance commit
`1b24e29` was reverted as `4fafc9f`. This restores the pre-audit fire-hook
startup order and all runtime probe/diagnostic state while retaining the later
asynchronous watch readback, CPU bone-palette snapshots, and horizon/6DOF/watch
trace gates. Fire, muzzle, decal, projectile, and physical-turn-related source
now has no diff from the original `98295fd` branch baseline.

Release x64 built with 0 warnings and 0 errors. Candidate:
`Builds/physical-turn-first-perf-checkpoint-rollback-20260825-0707/d3d11.dll`,
SHA-256 `9FF106113641926F777CEF33FCB28A23A48AE57E3AFABC8E56DB37C520800235`.
The immediately preceding failed-test DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`F68DF313D4AC592BC4D9F59FD809F1DA4E0BB72D2959A47772AF9006576B943E`.

### Exact pre-performance baseline shooting test

Reverting only the initial performance checkpoint did not change the physical-
turn failure. To avoid multiple headset runs through later changes that do not
touch shooting (watch sampling, left-hand palette snapshots, and log gates), an
exact detached worktree at `98295fd` was built for a decisive whole-audit A/B.
This is the branch state immediately before `codex/performance-audit` began and
therefore contains none of its source changes.

Candidate:
`Builds/physical-turn-exact-preperformance-98295fd-20260825-0720/d3d11.dll`,
SHA-256 `6CD16993375C93DA5E37EB8E16EC77A2C1D2B443CC6FC07771E7181F45EDCAD4`.
The preceding single-checkpoint rollback DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`9FF106113641926F777CEF33FCB28A23A48AE57E3AFABC8E56DB37C520800235`.

### Physical-turn shooting regression boundary — pre-recenter checkpoint

Two further hypotheses were rejected and reverted. Bypassing only the private
world-decal constant-buffer path did not change decals. Supplying an invalid
body anchor from Metro's `0x170` camera vector made shooting substantially
worse: after physical turning, rounds traveled only toward the original facing
direction. The new logs explained the failure—the camera vector alternated
between a real roughly 87-degree heading and a synthetic `(0,0,1)` placeholder
during firing, so it cannot be used as a stable world/body basis. The normal
decal and projectile behavior was rebuilt and restored as
`Builds/physical-turn-restored-baseline-20260825-1258/d3d11.dll`, SHA-256
`A4C48100381DEAFC4234E1DACED5AB621834C6118A2D3D8F7D435EB85B253D47`.

The next test is an exact historical boundary rather than another transform
guess. Detached commit `5b51602` is the August 22 checkpoint immediately before
`736723a` (`fix: align weapon and shots after VR recenter`), the only later
pre-regression commit explicitly changing weapon/shot transforms. It predates
the August 23-24 hand/watch/lighter work, so those features are not relevant to
this diagnostic; test only physical-turn shot accuracy and persistent bullet
holes. If this build works, the regression is bounded to `736723a` or later. If
it fails, the boundary must move back toward the August 17 validated shooting
checkpoint `72bf886`.

Exact detached candidate:
`Builds/physical-turn-pre-recenter-shooting-5b51602-20260825-1300/d3d11.dll`,
SHA-256 `422D5169BA531A4321B57EF4EC9005ECC0DB9005E74179230B3B068B3740C172`.
The restored current baseline is preserved beside it as `d3d11.pre-change.dll`,
SHA-256 `A4C48100381DEAFC4234E1DACED5AB621834C6118A2D3D8F7D435EB85B253D47`.

### Physical-turn shooting regression boundary — August 17 checkpoint

The exact `5b51602` pre-recenter build produced the same failure, ruling out
`736723a` and every later hand/watch/lighter or performance-audit change. A new
observation narrows the visual symptom: after physical turning, bullet holes
clip progressively at the top or bottom of the HMD view while looking up or
down, and other level geometry remains visible. Combined with the earlier log
evidence that the decal draw and persistent index count continue, this points
to a bullet-hole-specific visibility/frustum or shader clip input rather than
global scene culling or deletion of the decals.

The next exact historical boundary is detached commit `72bf886` (August 17,
`Checkpoint current VR implementation and shooting fixes`). This predates the
August 18-22 camera/culling sequence as well as all later features. Test only
physical-turn shot direction, bullet-hole persistence, and the vertical edge
clipping; missing or older unrelated features are expected. If this build
works, bisect the August 18-22 camera/culling commits. If it fails, stop moving
through recent history and instrument Metro's private decal visibility path.

Release x64 built successfully with 22 pre-existing warnings and 0 errors.
Exact detached candidate:
`Builds/physical-turn-validated-shooting-72bf886-20260825-1307/d3d11.dll`,
SHA-256 `0DD4C1B20734D7E5C3C094F7E09C425374C0099D0DB6543CF0B6E42D37CBCDDD`.
The failed `5b51602` boundary DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`422D5169BA531A4321B57EF4EC9005ECC0DB9005E74179230B3B068B3740C172`.

This August 17 boundary was not a valid decal test. In headset it restored the
early, incomplete resolution-slider implementation and corrupted the rendered
image into vertically stretched columns below the upper portion of the frame.
The later SSAA-specific resolution handling had not yet been completed at this
commit, so its bullet-hole behavior could not be evaluated reliably. Do not
classify `72bf886` as passing or failing the physical-turn symptom, and do not
deploy another whole historical DLL for this bisect without first transplanting
only the relevant camera/decal changes onto the current rendering baseline.

The later current-source rebuild was restored immediately for the 1x image
check, but was not yet revalidated above 1x:
`Builds/physical-turn-restored-baseline-20260825-1258/d3d11.dll`, SHA-256
`A4C48100381DEAFC4234E1DACED5AB621834C6118A2D3D8F7D435EB85B253D47`.

### Rejected: decal rasterizer scissor isolation

A current-baseline candidate disabled `ScissorEnable` only in the private
world-decal rasterizer state. In headset, that DLL instead started with the
whole image visibly zoomed, so it was rejected and reverted immediately. The
candidate's one-time diagnostic also recorded
`VRPose decal: private raster state native_scissor=0 forced_scissor=0`, proving
that Metro's incoming decal rasterizer already had scissoring disabled. The
intended state change was therefore a no-op and the native-scissor hypothesis
is ruled out independently of the zoomed session.

The archived current-source rebuild was restored and hash-verified at this
point, but the later resolution-slider check showed it was not a known-good
replacement:
`Builds/physical-turn-restored-baseline-20260825-1258/d3d11.dll`, SHA-256
`A4C48100381DEAFC4234E1DACED5AB621834C6118A2D3D8F7D435EB85B253D47`.

### Resolution-slider recovery — exact pre-investigation binary

Testing above `1.0x` exposed that the `A4C481...` current-source rebuild zooms
the HMD image when VR resolution is raised; checking only its hash and the 1x
image was not sufficient to call it known-good. The exact binary chain provides
the correct rollback point: the first bullet-hole candidate preserved its
incoming DLL as `d3d11.pre-change.dll`, and that file matches the final
headset-tested performance build byte-for-byte.

Restored exact pre-bullet-hole DLL:
`Builds/performance-6dof-trace-off-20260824-2356/d3d11.dll`, SHA-256
`C7044A5172CB7271F3C6D7652224CAFF16FD62EF2A995DFB7225D9A77AACA814`.
It is also archived at
`Builds/resolution-slider-recovery-20260825-1710/d3d11.dll`. The rejected
`A4C481...` rebuild is preserved there only for comparison and must not be used
as a rollback baseline. Validate VR resolution `1.5x` after a complete restart,
because the established resolution setting is restart-only.

The exact DLL alone did not recover the image because persistent Metro video
state had also changed. The AppData configuration had been saved as
`5760x3240`, the NVIDIA DSR/DLDSR mode that previously produced corrupted or
incorrect framing. An initial recovery attempt incorrectly copied Metro's
detected internal scene-canvas height (`2560x1421`) into `user.cfg`; that also
reproduced the zoom. Archived working logs prove these are intentionally
different dimensions: the saved display/swap-chain resolution is `2560x1440`,
while Metro's Off scene canvas is `2560x1421`.

The final recovery restored `user.cfg` to `2560x1440`, leaving
`r_enum_ssaa 10`, `r_supersample 1.0`, and the user's other settings unchanged.
Changing Metro SSAA to another option, restarting, then changing it back and
restarting forced a full video-device/render-target reset and cleared the stale
layout left by the rollback. A subsequent clean log again showed the expected
`2560x1440` swap chain, `2560x1421` scene canvas, and active high-resolution
overlay target. The `C7044A...` performance DLL was restored and headset-tested
successfully at both VR resolution `1.5x` and `2.0x` with correct framing.
Recovery backups and the clean post-reset log are in
`Builds/resolution-slider-recovery-20260825-1935/`.

### Physical-turn decals — native deferred-depth-space candidate

The confirmed bullet-hole pipeline revealed the missing relationship. Vertex
shader `C5A6A1F5B984FBF0` emits both final clip position from `m_VP` and
view-space position from `m_V`. Its pixel shader then samples the full-resolution
`t_position` G-buffer at the current screen pixel, subtracts the decal's
view-space Z, fades the result, and executes `discard_nz` when the two surfaces
do not overlap. The complete disassembly is preserved in
`GameReferences/decal_pipeline.txt` and can be reproduced with
`Tools/inspect_decal_pipeline.py`.

Regular world geometry intentionally preserves Metro's native `m_WV` so the
deferred position/normal buffers and lighting remain in one stable engine-view
space. The private decal buffer instead replaced both `m_V` and `m_VP` with the
HMD view. `m_VP` was correct for visible placement, but `m_V` made the pixel
shader compare HMD-view decal depth with native-view G-buffer depth. The two
spaces agree near Metro's body-facing camera and diverge under physical HMD yaw
or pitch, directly explaining the small visible window and progressive
left/right/top/bottom clipping while the draw and index count continue.

Candidate fix: capture Metro's unmodified main `m_V` before the camera patch.
For positively identified world-decal draws only, use that native view for
`m_V` while retaining `P_eye * V_new` for `m_VP`. The same split is applied to
the live eye, immediate twin pass, and batched second-eye replay. Small
shot-window muzzle-effect draws retain their existing path. No resolution,
global camera, culling, projectile, input, or performance code was changed.

Release x64 built successfully with 8 pre-existing warnings and 0 errors.
Deployed candidate:
`Builds/physical-turn-decal-native-depth-space-20260825-1925/d3d11.dll`,
SHA-256 `AC750B4647479002BAE9810F427D6B269637CDAAD1887A6FC977757B3B37878F`.
The exact headset-validated C704 baseline is preserved beside it as
`d3d11.pre-change-C704.dll`, SHA-256
`C7044A5172CB7271F3C6D7652224CAFF16FD62EF2A995DFB7225D9A77AACA814`.
Pre-test copies of `user.cfg` and `vr_menu_settings.txt` are included so the
known-good 1.5x persistent resolution state is recoverable independently of
the DLL.

Headset validation: the mixed-space regression is fixed. Persistent bullet
holes remain visible through physical left/right HMD rotation, and the severe
top/bottom clipping window is gone. A small residual remains only as holes
leave the extreme top or bottom edge of the visible HMD image. The correlated
runtime log still shows the decal draw and its persistent index count, while
the settled native pitch-follow error is generally below about half a degree;
this is therefore an extreme-frustum/normal clip-edge case, not recurrence of
the mixed-space depth discard.

Keep `0bbda63` / `AC750B...` as the accepted physical-turn decal fix. Do not
re-enable the old global 110-degree native-FOV override for this residual: that
path widens visibility for the entire scene (counterproductive to the current
performance audit), previously required a second global LOD-discard override,
and was retired once the targeted CPU visibility bypasses were established.
If the final edge sliver later proves distracting, isolate it as a decal-only
clip/depth-tolerance investigation without changing global camera projection,
resolution, or scene culling.

### Performance audit checkpoint 7 — retired camera-view traces

Profiling the log from the validated physical-turn decal test exposed two
remaining diagnostics in the main camera constant-buffer hot path. In that
single session, `VRPose pitchterms` and `VRPose viewstab` each emitted 3,344
lines. Their trace blocks also performed diagnostic-only trigonometry, body-
heading queries, residual normalization, and worst-value tracking; none of
their results are consumed by rendering.

This checkpoint adds one false release gate around those two related camera-
view diagnostics and skips the three pitch values calculated only for their
log. Actual camera composition, shake damping, culling follow, decals,
shooting, resolution, and SSAA code are unchanged.

Release x64 built with 0 warnings and 0 errors. Deployed candidate:
`Builds/performance-camera-view-trace-off-20260825-1945/d3d11.dll`, SHA-256
`2FF8A14CF45F4510CA246235B9BA77FE7336BDB1B17108773E5B2286F6CDB5E7`.
The accepted decal DLL is preserved beside it as `d3d11.pre-change.dll`,
SHA-256
`AC750B4647479002BAE9810F427D6B269637CDAAD1887A6FC977757B3B37878F`.
Matching symbols and pre-test copies of the AppData `user.cfg` and game-root
`vr_menu_settings.txt` are included. Deployment hash verification succeeded;
the persistent state remains `2560x1440`, VR resolution `1.5000`, preset 3.

Headset validation: normal gameplay, physical turning/bullet holes, watch, and
the 1.5x resolution path remained correct. The following runtime log contained
zero `VRPose pitchterms` and zero `VRPose viewstab` lines, confirming the gate
is active. Keep `387ca32` / `2FF8A14C...` as accepted checkpoint 7.

### Performance audit checkpoint 8 — retired roll diagnostics

The checkpoint-7 test log exposed a larger historical diagnostic pair. It
contained 15,959 `VRPose roll` lines plus 178 companion delta-rotation matrix
dumps. The engine-roll half runs in Metro's repeatedly mapped camera-buffer
path and performed a clamp and `asin` before each emitted line. The matching
HMD/delta half was used only to correlate the source of a now-solved horizon-
tilt problem. Neither result is consumed by the accepted horizon correction.

This checkpoint gates only the paired engine/HMD roll measurements and their
matrix dump. Camera/horizon composition, shake damping, culling, shooting,
decals, resolution, and SSAA code are unchanged.

Release x64 built with 0 warnings and 0 errors. Deployed candidate:
`Builds/performance-roll-trace-off-20260825-1958/d3d11.dll`, SHA-256
`8D5A412B709CA8BF9CB2E8B3F21964BE686D07E83B2311251DE5DBA7C6003334`.
The headset-validated checkpoint-7 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`2FF8A14CF45F4510CA246235B9BA77FE7336BDB1B17108773E5B2286F6CDB5E7`.
Matching symbols and the unchanged resolution-setting snapshots are included.

Headset validation: horizon/head tracking, physical turning and bullet holes,
watch behavior, and 1.5x framing remained correct. The following runtime log
contained zero `VRPose roll` and zero delta-rotation matrix lines, confirming
both halves of the gate are active. Keep `f352abd` / `8D5A412B...` as accepted
checkpoint 8.

### Performance audit checkpoint 9 — weapon-translation traces

The checkpoint-8 log exposed four related probes left from the solved weapon-
depth investigation. In that short run they emitted 3,419 corrected-grip
lines, 6,837 controller/weapon-depth lines, 1,319 raw translation-ratio lines,
and 1,319 derived-ratio lines: 11,894 total. The corrected-grip probe also
performed diagnostic-only matrix transforms. None of these values feeds the
accepted controller or weapon placement.

This checkpoint gates only those four weapon-translation measurements and
logs. Controller pose acquisition, weapon transforms, aiming/shooting, camera
and horizon transforms, culling, decals, resolution, and SSAA are unchanged.

Release x64 built with 0 warnings and 0 errors. Deployed candidate:
`Builds/performance-weapon-translation-trace-off-20260825-2004/d3d11.dll`,
SHA-256
`3479634C738472DDCA5BBE4B62A10058DC684D808EFF9897F20DDEAC4BCCD600`.
The headset-validated checkpoint-8 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`8D5A412B709CA8BF9CB2E8B3F21964BE686D07E83B2311251DE5DBA7C6003334`.
Matching symbols and the unchanged resolution-setting snapshots are included.

### Physical-turn shot zero — exact rendered-view-basis candidate

The user's `2026-08-25 20-18-03.mp4` exposed a smaller shooting issue after
the decal fix made impacts observable through physical turns. With the weapon
held on its sights, impacts drift slightly to opposite sides under left versus
right physical HMD yaw and return to the sights at the original facing
direction. The weapon itself continues to follow the controller correctly.

This was not introduced by performance checkpoint 9: that checkpoint only
placed false release gates around four log/measurement blocks and does not
alter any pose or shot value. The earlier whole-audit A/B also found physical-
turn shot error in the exact pre-performance branch, although the severe decal
clipping present then obscured this smaller residual.

The remaining relationship is in `GetCurrentRenderedViewBasis`. The rendered
gun uses Metro's exact completed view after culling-follow cancellation. That
view contains the small signed residual left when Metro's camera does not
respond exactly to the cancellation booked for the frame. The shot instead
converted its view-space barrel through a reconstructed ideal `body + head`
basis, omitting the residual. The two bases therefore agree at center and
diverge with opposite sign under left/right physical yaw—the exact video
pattern.

Candidate fix: use the already-published `vrCachedView3x4` rotation, the exact
basis that drew the gun, for view-to-world shot conversion. Retain the ideal
reconstruction only as a startup fallback before the main camera publishes a
completed view. The same helper feeds the legacy inline redirect and current
projectile-creation redirect, keeping all weapons on one basis. Controller and
weapon transforms, zeroing, native spread, decals, performance gates,
resolution, and SSAA are unchanged.

Release x64 built with 0 warnings and 0 errors. Deployed candidate:
`Builds/physical-turn-shot-rendered-view-basis-20260825-2038/d3d11.dll`,
SHA-256
`9EAC201E4DA12DC20DADB46E12A2FAB7D27C671184C6DD757939668F19242F90`.
Performance checkpoint 9 is preserved beside it as `d3d11.pre-change.dll`,
SHA-256
`3479634C738472DDCA5BBE4B62A10058DC684D808EFF9897F20DDEAC4BCCD600`.
Matching symbols and unchanged persistent resolution snapshots are included.

#### Headset result and next isolation — 6DOF projectile origin

Headset testing reported no change from the exact rendered-view-basis
candidate. That result rejects the cached-versus-reconstructed rotation as the
cause, and the source change was reverted before the next candidate.

The correlated log rules out native spread as the hidden offset in this test.
All recorded projectile-creation calls had `aiming=1` and
`dispersionScale=0.000`, so the measured native deviation was not added to the
replacement direction. The replacement directions themselves followed the
physical turn correctly, including world headings near plus and minus 90
degrees. Direction rotation is therefore functioning even while the impact is
slightly displaced from the sights.

The remaining transform mismatch is the projectile origin. Metro supplies an
origin in its body-camera frame, while `PatchMappedVRCameraData` renders from a
camera translated by the headset's tracked 6DOF displacement. Physical yaw
moves the HMD several centimetres around the original standing point. At the
short wall distances in the video, leaving the projectile at Metro's unshifted
origin produces signed parallax under left/right physical turns and naturally
returns to zero at the original pose.

Candidate fix: preserve Metro's native projectile origin and add only the same
tracked translation already used by the rendered camera, rotated into world
space by the live engine-camera heading. Apply the origin adjustment only when
the existing projectile direction redirect succeeds. Projectile direction,
weapon/controller transforms, zeroing, native dispersion, decals, performance
gates, resolution, and SSAA are unchanged.

Release x64 built successfully with eight pre-existing warnings and zero
errors. Deployed candidate:
`Builds/physical-turn-shot-6dof-origin-20260825-2052/d3d11.dll`, SHA-256
`D63D3E3F67F1BA722BE833CA0B7F961FCDD54423190662EB9C73234BE6067579`.
The failed rendered-basis DLL and clean performance checkpoint 9 are preserved
beside it as `d3d11.pre-change.dll` and `d3d11.checkpoint9.dll`. Unchanged
persistent resolution snapshots are included.

Headset validation: fixed. ADS shots remain aligned after physical left/right
rotation and when returning to the original facing direction. Keep `07fc61c` /
`D63D3E3F...` as the accepted physical-turn shot-origin fix.

The separated-left-hand work did not introduce this defect. Git history dates
the rendered-camera 6DOF translation to `6fc4b7a` on 2026-08-14; its source
comment explicitly states that weapon and shot-origin logic were still
unchanged. The left hand/watch split arrived later in `8d655c8` on 2026-08-23
and does not modify the right-hand weapon or projectile origin. The issue was
therefore latent from the earlier 6DOF camera stage and became observable once
the decal fix made physical-turn impacts reliable to inspect.

### Performance audit checkpoint 10 — retired resolution diagnostics

The log from the validated physical-turn shooting test exposed the largest
remaining trace family. It contained 19,687 `VR resolution diagnostic` lines,
including 19,538 viewport entries, plus 3,702 high-resolution overlay-target
activation lines. The same retired investigation also queried COM resource
interfaces and texture descriptions from copy, resolve, render-target-binding,
and final-pass draw paths solely to produce diagnostic output.

This checkpoint places one false release gate around that diagnostic family in
`HackerContext.cpp` and `HackerDevice.cpp`. The resolution setter and all
functional resolution paths remain active: scene sizing, final-colour capture,
high-resolution overlay routing, viewport/scissor scaling, SSAA handling, and
OpenVR submission are unchanged. Camera, shooting, decals, watch, controller,
and gameplay code are also unchanged.

Release x64 built with 0 warnings and 0 errors. Deployed candidate:
`Builds/performance-resolution-diagnostics-off-20260825-2103/d3d11.dll`,
SHA-256
`63F050CC6C4AF741BAC559E62D0B180802DDA881C15D7784928335B5F254F157`.
The headset-validated physical-turn shooting DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`D63D3E3F67F1BA722BE833CA0B7F961FCDD54423190662EB9C73234BE6067579`.
Matching symbols and pre-test copies of the unchanged 1.5x persistent
resolution state are included.

Headset validation: normal gameplay and the 1.5x resolution path remained
correct. The following runtime log contained zero retired resolution-diagnostic
or high-resolution overlay-activation trace lines, confirming the gate is
active. Keep `c80719a` / `63F050CC...` as accepted checkpoint 10.

### Performance audit checkpoint 11 — repeated LoadLibrary redirect trace

The checkpoint-10 validation log contained 9,102 identical
`Replaced Hooked_LoadLibraryExW` lines. They came from the success path that
redirects NVAPI's repeated System32 load request to the wrapper in Metro's game
directory. The redirect is required, but logging every successful call is not.

This checkpoint places a false release gate around only that success message.
Path construction and comparison, the actual LoadLibrary call, its return
value, fallback behavior, and fallback/error logging are unchanged. Rendering,
resolution/SSAA, camera, shooting, decals, watch, controller, and gameplay code
are unchanged.

Release x64 built with 0 warnings and 0 errors. Deployed candidate:
`Builds/performance-loadlibrary-trace-off-20260825-2111/d3d11.dll`, SHA-256
`5FCEA96ADA40B51DFE6E3776F1ABC6EFCBBB1908EEFFB30219DFB04440C71965`.
The headset-validated checkpoint-10 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`63F050CC6C4AF741BAC559E62D0B180802DDA881C15D7784928335B5F254F157`.
Matching symbols and unchanged 1.5x persistent resolution snapshots are
included.

Headset validation: startup and normal gameplay remained correct. The
following runtime log contained zero repeated LoadLibrary redirect-success
lines, confirming the gate is active. Keep `36da6f9` / `5FCEA96A...` as
accepted checkpoint 11.

### Performance audit checkpoint 12 — watch icon-anchor trace

The checkpoint-11 validation log exposed 10,099 `VRPose watch UI: icon anchor
matched` lines, now the single largest remaining trace. The icon detector and
its state updates are functional—they arm the native-glyph decoder and schedule
the asynchronous watch capture—but the success message is not consumed.

This checkpoint gates only that message. Watch-icon shader matching, block
arming, glyph ordinals, capture scheduling, asynchronous glyph capture,
native-glyph suppression, and custom 3D watch rendering are unchanged.
Resolution/SSAA, camera, shooting, decals, controller, and gameplay code are
also unchanged.

Release x64 built with 0 warnings and 0 errors. Deployed candidate:
`Builds/performance-watch-anchor-trace-off-20260825-2117/d3d11.dll`, SHA-256
`72FD8613344964CBDEA32FAC28E23B6E8CFF383B31801549A8989BD9D08FAB62`.
The headset-validated checkpoint-11 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`5FCEA96ADA40B51DFE6E3776F1ABC6EFCBBB1908EEFFB30219DFB04440C71965`.
Matching symbols and unchanged 1.5x persistent resolution snapshots are
included.

Headset validation: the custom watch remained correct. The following runtime
log contained zero `icon anchor matched` lines, confirming the trace gate is
active. Keep `4006234` / `72FD8613...` as accepted checkpoint 12.

### Close-range authored decal disappearance candidate

Video `2026-08-25 21-22-42.mp4` shows several level-authored surface layers
disappearing as the camera approaches them in the mess hall. The underlying
wall and sign meshes stay visible: the decorated face of each billboard pops
to a blank face, and a nearby light-like or emissive contribution disappears
in the same proximity pattern. This is not a whole-object or room-portal
failure. The validation log also confirms that the targeted CPU screen,
object, and cluster visibility bypasses are installed.

Checkpoint 12 cannot cause this symptom; its only runtime change gates one
watch-anchor `LogInfo` call. The evidence instead matches the known shared
world-decal path. Metro uses the same `kMuzzleFlashVS` family for the weapon
flash and world-space decals, including authored surface overlays. The source
already documents that many decals failed hardware depth testing when viewed
close to their receiving surface. The accepted physical-turn fix keeps the
decal pixel shader in Metro's native depth space, but it deliberately retained
the earlier `0.05` clip-space separation and did not retune the remaining
close-range hardware-depth margin.

Isolated candidate: increase only the existing pre-divide decal clip-space
depth separation from `0.05` to `0.10`, with one shared constant used by both
the live-eye and replayed twin-eye paths. Native decal depth space and the
physical-turn fix are unchanged. Resolution/SSAA, camera and object culling,
watch, shooting, controller transforms, persistent settings, and gameplay code
are unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/close-range-world-decal-depth-bias-20260825-2145/d3d11.dll`, SHA-256
`20A83BED97CCF3C30E57141EA4D2CC936C3BEAACC4C41D0FD481BD3D3A833EA5`.
The headset-validated checkpoint-12 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`72FD8613344964CBDEA32FAC28E23B6E8CFF383B31801549A8989BD9D08FAB62`.
Matching symbols and unchanged 1.5x persistent resolution snapshots are
included.

Headset validation: fixed. The mess-hall billboard overlays and the associated
light-like projected layers no longer disappear when approached. Keep
`734d252` / `20A83BED...` as the accepted close-range world-decal depth fix and
the new known-good rendering checkpoint.

### Performance audit checkpoint 13 — main-camera patch trace

The validated close-range decal run contained 1,560 `VRPose: patched camera
buffer` lines in approximately 5,600 frames, making it the largest remaining
trace family. Its local counter and periodic message run inside the main camera
constant-buffer patch, but neither value is consumed by rendering.

This checkpoint places the counter and message inside the existing false
camera-view diagnostics gate. The Release optimizer removes the complete
bookkeeping block. Main-camera recognition, native decal-view capture, tracked
rotation, 6DOF translation, projection, culling, and the actual constant-buffer
writes are unchanged. Resolution/SSAA, decals, watch, shooting, controller
transforms, persistent settings, and gameplay code are also unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-camera-patch-trace-off-20260825-2202/d3d11.dll`, SHA-256
`D9CCFFD470820D4AADA4296C4FD9179A20376B5E0ECEE7EF0B27ECB52D847CE4`.
The headset-validated close-range decal DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`20A83BED97CCF3C30E57141EA4D2CC936C3BEAACC4C41D0FD481BD3D3A833EA5`.
Matching symbols and unchanged 1.5x persistent resolution snapshots are
included.

Headset validation: normal gameplay remained correct, and the following
runtime log contained zero `VRPose: patched camera buffer` lines. Keep
`2f26a63` / `D9CCFFD4...` as accepted checkpoint 13.

### Performance audit checkpoint 14 — watch glyph-queue trace

The checkpoint-13 validation log contained 881 `VRPose watch UI: queued glyph`
lines, now the largest remaining single trace family. The message is emitted
after each captured glyph's texture hash and dimensions have already been
stored; no functional code consumes the message.

This checkpoint gates only that queue-success message. Watch glyph matching,
texture assignment, asynchronous capture, decoding, native-glyph suppression,
and custom 3D rendering are unchanged. Resolution/SSAA, camera, decals,
shooting, controller transforms, persistent settings, and gameplay code are
also unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-watch-glyph-queue-trace-off-20260825-2209/d3d11.dll`,
SHA-256
`BE179F4C48542E0B17F4D6D917479EB69ED971754BC1FA92062F604D3FB2F016`.
The headset-validated checkpoint-13 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`D9CCFFD470820D4AADA4296C4FD9179A20376B5E0ECEE7EF0B27ECB52D847CE4`.
Matching symbols and unchanged 1.5x persistent resolution snapshots are
included.

Headset validation: the watch and ordinary gameplay remained correct. The
following runtime log contained zero `queued glyph` messages, confirming the
gate is active. Keep `ca56b0c` / `BE179F4C...` as accepted checkpoint 14.

### Performance audit checkpoint 15 — hand/watch attachment diagnostics

The checkpoint-14 validation log contained 2,157 lines from one retired
attachment-measurement family: 872 `handstage`, 436 `handdiag`, 436
`handmatrix`, and 413 `watchrel` messages. In addition to formatting those
lines, the blocks repeatedly evaluated a diagnostic-only fingertip from the
bone palette, calculated multiple rigidity metrics and square roots, called
`acos`, projected the wrist through both eyes, inverted the completed hand
matrix, and multiplied the watch-relative result.

This checkpoint places only those measurement blocks behind a false release
gate and moves the diagnostic-only fingertip evaluation inside it. Functional
hand-centre and wrist evaluation, wrist placement, controller transforms,
private bone-palette writes, watch parenting/calibration, and all attachment
render paths are unchanged. Resolution/SSAA, camera, decals, shooting,
persistent settings, and gameplay code are also unchanged.

Release x64 built with 0 warnings and 0 errors. The optimized DLL is 6,144
bytes smaller, consistent with the unreachable measurement code being removed.
Candidate archive:
`Builds/performance-hand-watch-diagnostics-off-20260825-2217/d3d11.dll`,
SHA-256
`71CF4B889B41F6E0DDB3369BC4963E72B742894DE533C3A395AA589B2F5F9AB0`.
The headset-validated checkpoint-14 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`BE179F4C48542E0B17F4D6D917479EB69ED971754BC1FA92062F604D3FB2F016`.
Matching symbols and pre-test persistent-setting snapshots are included.
Metro changed only the opaque `xbox_net_data` progression field in `user.cfg`;
the verified 2560x1440, SSAA 10, supersample 1, fullscreen-off, and 1.5x VR
resolution values remain unchanged.

Headset validation: the left hand, watch, lighter, journal, controller motion,
and ordinary gameplay remained correct. The following runtime log contained
zero `handstage`, `handdiag`, `handmatrix`, or `watchrel` messages, confirming
the release gate is active. Keep `190a5dc` / `71CF4B88...` as accepted
checkpoint 15.

### Performance audit checkpoint 16 — universal-UI diagnostics

The checkpoint-15 validation log contained 770 lines from the retired
universal-UI measurement family: 320 `uiplace`, 225 `UI gate`, and 225 `UI hit`
messages. The probes also recalculated the transformed canvas centre and
performed shader-map lookups solely to report a diagnostic label.

This checkpoint puts those three measurement blocks behind one false release
gate. UI draw classification, the captured classifier state, per-eye
projection, HUD/menu placement, constant-buffer updates, watch capture, and
rendering remain active and unchanged. Resolution/SSAA, camera, decals,
shooting, controller transforms, persistent settings, and gameplay code are
also unchanged.

Release x64 built with 0 warnings and 0 errors. The optimized DLL is 1,536
bytes smaller, consistent with removal of the unreachable diagnostic blocks.
Candidate archive:
`Builds/performance-universal-ui-diagnostics-off-20260825-2228/d3d11.dll`,
SHA-256
`AF90D50A8D554EE4932503CBAAF6B04E04B6654B34608BF41940F54D87ECEA73`.
The headset-validated checkpoint-15 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`71CF4B889B41F6E0DDB3369BC4963E72B742894DE533C3A395AA589B2F5F9AB0`.
Matching symbols and unchanged persistent-setting snapshots are included.

Headset validation: HUD placement, pause/equipment/weapon menus, watch
rendering, and ordinary gameplay remained correct. The following runtime log
contained zero `uiplace`, `UI gate`, or `UI hit` messages, confirming the gate
is active. Keep `1574335` / `AF90D50A...` as accepted checkpoint 16.

### Performance audit checkpoint 17 — native watch-glyph suppression trace

The checkpoint-16 validation log contained 650 `VRPose watch UI: suppressed
captured HH:MM glyph` messages, now the largest remaining trace family. The
counter and formatted message are diagnostic only; the functional native-glyph
suppression occurs afterward through the independent `skip = true` path.

This checkpoint gates only that counter and message. Watch capture, decoding,
texture tracking, timer fallback, native/custom selection, and custom 3D watch
rendering remain active and unchanged. Resolution/SSAA, UI placement, camera,
decals, shooting, controller transforms, persistent settings, and gameplay
code are also unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-watch-suppression-trace-off-20260825-2238/d3d11.dll`,
SHA-256
`F4115F641DA800D8C18DF0B0CDF0ABC68EFA99BC1281EDB1611B34A1A8D5D5D9`.
The headset-validated checkpoint-16 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`AF90D50A8D554EE4932503CBAAF6B04E04B6654B34608BF41940F54D87ECEA73`.
Matching symbols and unchanged persistent-setting snapshots are included.

Headset validation: custom watch time/timer remained correct with no duplicate
native digits. The following runtime log contained zero `suppressed captured
HH:MM glyph` messages, confirming the gate is active. Keep `730470b` /
`F4115F64...` as accepted checkpoint 17.

### Performance audit checkpoint 18 — single-pass flicker traces

The checkpoint-17 validation log contained 146 `SinglePass candidate` and 16
`SinglePass FOLDED` messages. Their retired flicker-trace blocks also performed
pixel-shader hash lookup/fallback work and maintained two `unordered_set`
collections from the single-pass draw-routing hot path.

This checkpoint places only those two trace blocks behind one false release
gate. The functional vertex-shader lookup, fullscreen-video exclusion,
forced-double-draw rules, target/depth checks, sampled-eye-resource rejection,
variant selection, single-pass statistics, and actual folded rendering remain
active and unchanged. Resolution/SSAA, UI/watch rendering, camera, decals,
shooting, controller transforms, persistent settings, and gameplay code are
also unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-singlepass-traces-off-20260825-2246/d3d11.dll`, SHA-256
`94B9D321A978CBCE24736BC8A7D9B98B3EC955EBAC645233DE552D9228EF7C9F`.
The headset-validated checkpoint-17 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`F4115F641DA800D8C18DF0B0CDF0ABC68EFA99BC1281EDB1611B34A1A8D5D5D9`.
Matching symbols and unchanged persistent-setting snapshots are included.

Headset validation: ordinary scenery, water, shadows, effects, UI/watch
rendering, and gameplay remained correct. The following runtime log contained
zero `SinglePass candidate` and `SinglePass FOLDED` messages, confirming both
trace gates are active. Keep `1a75f8b` / `94B9D321...` as accepted checkpoint
18.

### Performance audit checkpoint 19 — reticle draw trace

The checkpoint-18 validation log contained 1,554 `VRPose reticle: draw
detected` messages, the largest remaining trace family. This checkpoint gates
only that formatted trace/counter and removes one unused diagnostic sample
counter increment from each native reticle draw.

The native-reticle vertex readback is deliberately preserved. It is a
functional, triple-buffered, non-blocking path that updates the replacement
reticle's live recoil/spread; removing it would sacrifice visual and gameplay
feedback. Reticle classification, native suppression, custom rendering,
redirected-shot direction, and per-eye projection remain active and unchanged.
Resolution/SSAA, UI/watch rendering, camera, decals, controller transforms,
persistent settings, and gameplay code are also unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-reticle-trace-off-20260825-2308/d3d11.dll`, SHA-256
`092A4D3EDCBA435F2AA68E7E90703AC276C91B1958573E89C71F8771FE4D9D8F`.
The headset-validated checkpoint-18 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`94B9D321A978CBCE24736BC8A7D9B98B3EC955EBAC645233DE552D9228EF7C9F`.
Matching symbols and current persistent-setting snapshots are included.

Headset validation: the custom reticle remained correctly aimed with no native
duplicate, and its marks still expanded and contracted with live recoil/spread.
Image scale and ordinary gameplay also remained correct. The following runtime
log contained zero `VRPose reticle: draw detected` messages, confirming the
trace gate is active. Keep `26098fc` / `092A4D3E...` as accepted checkpoint 19.

### Performance audit checkpoint 20 — watch decode trace

The checkpoint-19 validation log contained 384 `VRPose watch decode` messages.
Each was formatted and written after a complete four-digit batch had already
been published. This checkpoint gates only that repeated message.

The asynchronous staging-buffer capture/poll, atlas-cell matching, complete
batch validation, decoded digit state, validated texture commitment, native
glyph suppression, custom 3D watch rendering, and timer fallback remain active
and unchanged. Resolution/SSAA, reticle, UI, camera, decals, shooting,
controller transforms, persistent settings, and gameplay code are also
unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-watch-decode-trace-off-20260825-2316/d3d11.dll`, SHA-256
`14C3C7C36EE01101F0F9A8C299A9C520E0473DD7F072D4867329D52DC7BE767D`.
The headset-validated checkpoint-19 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`092A4D3EDCBA435F2AA68E7E90703AC276C91B1958573E89C71F8771FE4D9D8F`.
Matching symbols and current persistent-setting snapshots are included.

Headset validation: the custom 3D watch continued to show and update the
correct live time/timer with no native HMD duplicate. The reticle and ordinary
gameplay also remained correct. The following runtime log contained zero
`VRPose watch decode` messages, confirming the trace gate is active. Keep
`1bff862` / `14C3C7C3...` as accepted checkpoint 20.

### Performance audit checkpoint 21 — decal qualification observer

A preceding representative shooting log contained 623 `VRPose decal QUAL`
messages. The retired block performed a shader-map lookup from every ordinary
indexed draw solely to sample whether matching decal draws used the batch or
inline twin path, then updated a counter and wrote the result. This checkpoint
places that entire observer block behind a false release gate.

Left-eye rendering, batch eligibility, right-eye inline twin fallback, actual
stereo routing, muzzle-flash/decal classification, native decal view capture,
per-eye matrices, constant-buffer binding, close-range depth bias, bullet-hole
corrections, and physical-turn shooting remain active and unchanged.
Resolution/SSAA, reticle, UI/watch, camera, controller transforms, persistent
settings, and gameplay code are also unchanged.

Release x64 built with 0 warnings and 0 errors. The optimized DLL is 512 bytes
smaller, consistent with removal of the unreachable observer. Candidate
archive:
`Builds/performance-decal-qualification-trace-off-20260825-2319/d3d11.dll`,
SHA-256
`EC886637C454328E93780F1F1C782951B85A791E5CA38EA9DE66B6D0D9D0F916`.
The headset-validated checkpoint-20 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`14C3C7C36EE01101F0F9A8C299A9C520E0473DD7F072D4867329D52DC7BE767D`.
Matching symbols and current persistent-setting snapshots are included.

Headset validation: bullet holes remained visible, correctly clustered, and
stable before and after physical turning, including at close range. Shot
direction, muzzle flash, reticle, watch, image scale, and ordinary gameplay
also remained correct. The following runtime log contained zero `VRPose decal
QUAL` messages, confirming the observer gate is active. Keep `4afa8e2` /
`EC886637...` as accepted checkpoint 21.

### Performance audit checkpoint 22 — remaining decal verification traces

The checkpoint-21 shooting log contained 183 `VRPose decal TWIN` messages and
92 `VRPose decal: corrected` messages. The twin trace additionally called
`VSGetConstantBuffers` and acquired/released a COM reference solely to verify
the constant-buffer bind that the functional path had just issued. This
checkpoint gates both retired verification traces.

The required `VSSetConstantBuffers` bind, per-eye matrix updates, rasterizer
state cloning, close-range depth bias, state restoration, decal/flash
classification, stereo draw routing, physical-turn shot direction,
bullet-hole placement, and culling corrections remain active and unchanged.
Resolution/SSAA, reticle, UI/watch, camera, controller transforms, persistent
settings, and gameplay code are also unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-decal-verification-traces-off-20260825-2325/d3d11.dll`,
SHA-256
`2D331B46C6CF6C9FF4CD95CF49DF7CE1A7F778AEFAA464C598BE59A402702594`.
The headset-validated checkpoint-21 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`EC886637C454328E93780F1F1C782951B85A791E5CA38EA9DE66B6D0D9D0F916`.
Matching symbols and current persistent-setting snapshots are included.

### Muzzle companion-tail investigation — 2026-08-25

The player video `2026-08-25 23-30-07.mp4` exposed a separate issue while
validating checkpoint 22. The player clarified the decisive behavior: the
elongated orange component is separate from the beginning and follows Metro's
game-camera firing direction rather than the physical weapon. It is not a
correctly attached flash which merely stops following the gun during its fade.

Checkpoint 22 changed only two false-gated decal verification blocks after the
functional constant-buffer binds; it did not modify the muzzle classifiers,
weapon affine, particle matrices, tail timing, or draw routing. The current
companion classifier and 30-frame release-tail logic also blame back to the
headset-validated 2026-08-19 muzzle work. This therefore needs a fresh runtime
identity check rather than assuming the latest trace cleanup caused it.

Temporary diagnostic archive:
`Builds/diagnostic-muzzle-tail-signatures-20260825-2340/d3d11.dll`, SHA-256
`8DF83C1F65A524C61C74292C2003F78846319607F571EA972E111C63D5763D55`.
It adds only a bounded signature trace for six-index, 1-16-instance particle
draws during the shot/release window. The trace records VS/PS identity, exact
classifier result, map-time folded state, instance count, cached position, and
viewport. Rendering and gameplay behavior are unchanged. Checkpoint 22 is
preserved beside it for immediate rollback.

The completed trace isolated the missing pass. On the actual shot frames, one
additional six-index, one-instance particle draw appears with VS
`2403427473C35A13` and PS `37A83048AA387121`. Its cached world-view translation
remains on Metro's native game-camera path and the existing exact muzzle
classifier rejects it. The same shader family also renders unrelated persistent
4/5-instance batches, so neither shader identity nor particle layout is safe by
itself.

The correction therefore adds only that exact VS/PS pair at exactly one
instance to `IsMuzzleBillboardDraw`; the existing `IsMuzzleEffectActive` shot
window remains mandatory. This sends the live draw and its recorded second-eye
replay through the already-validated weapon-relative muzzle matrices without
touching the unrelated larger batches, lighter flame, ambient particles,
impacts, decals, shooting, or resolution handling. The temporary 1,200-line
signature trace was removed from the candidate.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/muzzle-companion-surface-pass-fix-20260826-0000/d3d11.dll`, SHA-256
`A4AEF20B9036053CF65C8B143D2A31D7AA1A2F8737FDAFF42072DD46F49C9241`.
The diagnostic runtime is preserved beside it as `d3d11.pre-change.dll`,
SHA-256
`8DF83C1F65A524C61C74292C2003F78846319607F571EA972E111C63D5763D55`.
Matching symbols and unchanged persistent-setting snapshots are included. The
candidate and symbols were deployed byte-for-byte; `user.cfg` and
`vr_menu_settings.txt` retained their pre-test hashes.

### Performance audit checkpoint 23 — retired muzzle-hunt observer

The remaining detached orange firing component is explicitly shelved for a
later focused investigation. A broader dynamic-quad classifier did not remove
it and was rejected; the game DLL and source classifier were restored to the
exact preceding candidate before resuming the performance audit.

The newest runtime log showed that the temporary muzzle hunt had been left
enabled during that investigation. Although its reporting is bounded, it
constructs and queries a draw-signature set from every indexed and instanced
draw. This checkpoint changes only `kFlashHunt` to false, compiling that
observer out. Muzzle rendering, the existing partial companion classifiers,
shot direction, decals, resolution/SSAA, UI/watch, controller transforms, and
gameplay behavior are unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-muzzle-hunt-off-20260826-1641/d3d11.dll`, SHA-256
`C14D3884FA584173F881D83BBF0ECEF8CA8F844837026D6262B327DA1119D520`.

Headset validation: both weapons and ordinary gameplay remained correct aside
from the separately shelved companion component. Keep checkpoint 24 active.

### Performance audit checkpoint 25 — prologue split-mesh trace

The same short runtime log contained 233 `VRPose prologue: split opening mesh`
messages—one formatted disk write for every frame in which the opening arm mesh
was split. This checkpoint false-gates only that repeated confirmation.

Opening-sequence recognition, the right-hand and left-hand index-range draws,
sleeve removal, controller attachment, stereo duplication, and all other
rendering/gameplay behavior remain unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-prologue-split-trace-off-20260826-1647/d3d11.dll`, SHA-256
`630B84F0E343033CD0CC4812CBE18259FB942CF59448F5EB464C0D35D138D7BE`.

Headset validation: shooting, particles, and ordinary gameplay remained
correct. Keep checkpoint 26 active.

### Performance audit checkpoint 27 — weapon draw diagnostic

The preceding runtime log reached the 60-entry cap of the old 336-index weapon
draw diagnostic. For matching draws it inspected saved matrices, calculated a
distance, selected a diagnostic tag, and formatted as many as 24 matrix values;
otherwise it repeatedly reported that no patch occurred. This checkpoint
false-gates that entire observer in the indexed-draw hot path.

Weapon recognition, instance-buffer substitution, controller-relative
transforms, hidden-viewmodel filtering, stereo routing, and all other
rendering/gameplay systems remain unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-weapon-draw-diagnostic-off-20260826-1656/d3d11.dll`,
SHA-256
`82149064E4488D3BA78607E78B75E1D954D6E2BF52517D15A904A637B4688473`.

During validation the player noticed physical-turn shot alignment was again
off, but could not identify which recent checkpoint exposed it. Source and
runtime verification confirmed that the accepted `07fc61c` correction is
still present: `kProjectileCreationRedirect` is true, the +0x3D7FF0 hook
installs, `BuildVRWorldProjectileOrigin` is byte-for-byte identical to the
accepted helper, and live shots report both `redirected=1` and
`originRedirected=1`. The later investigation should therefore focus on the
tracking/reference translation supplied to that helper; observed origin shifts
rose from about 0.017 m to 0.256–0.270 m after physical movement. Projectile
telemetry remains enabled for that work. The performance audit continues on
unrelated observers at the player's request.

### Performance audit checkpoint 29 — ADS hand-height trace

The latest runtime log contained 280 periodic `VRPose ads: hand-height`
messages. The functional near-face ADS hysteresis uses only the already
computed vertical height; the message additionally calculated a three-axis
square-root reach value solely for logging. This checkpoint false-gates its
counter, square root, and formatted output.

Controller/HMD sampling, near-face thresholds and hysteresis, native ADS state,
shot hooks and telemetry, weapon transforms, and all rendering/gameplay
behavior remain unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-ads-hand-height-trace-off-20260826-1813/d3d11.dll`,
SHA-256
`860243DC5C0C09F652AE50E826D8D8DF05D972673C70C5DFA8DBCEC2C4E0AD42`.

### Performance audit checkpoint 30 — LOD census

The LOD observer maintained two `unordered_set` instances of index counts,
copied and cleared them each frame, and compared both sets to calculate mesh
churn. It was diagnostic-only and did not select or alter any LOD. This
checkpoint disables that per-frame set work and its periodic message.

Metro's native LOD selection, draw submission, culling, transforms, and all
visual/gameplay behavior remain unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-lod-census-off-20260826-1820/d3d11.dll`, SHA-256
`8785060BD5ED6AA969A0AB3AEA8744CAC42A1261661BAFA5ACF4F66F600FC854`.

Headset validation: close-range visibility, shooting, and ordinary gameplay
remained correct. Keep checkpoint 30 active.

### Performance audit checkpoint 31 — flicker census

The flicker observer incremented a counter on every intercepted draw and
maintained a 120-frame min/max window before periodically writing its summary.
It was diagnostic-only and did not affect culling, visibility, or submission.
This checkpoint disables that per-draw counter and aggregation.

All draw routing, culling/visibility behavior, transforms, stereo handling, and
visual/gameplay behavior remain unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-flicker-census-off-20260826-1823/d3d11.dll`, SHA-256
`9857A0FA88E383F17E260BD21302EE4F3435825E5BFC3933F088E0C0B6DBCFD9`.
The headset-validated checkpoint-30 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`8785060BD5ED6AA969A0AB3AEA8744CAC42A1261661BAFA5ACF4F66F600FC854`.
The headset-validated checkpoint-29 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`860243DC5C0C09F652AE50E826D8D8DF05D972673C70C5DFA8DBCEC2C4E0AD42`.
The checkpoint-28 DLL is preserved beside it as `d3d11.pre-change.dll`,
SHA-256
`25F2E2952EAF5C282E2D5EC20299A745D857950964C7BA4E4171CD8C3BB1F58C`.
The headset-validated checkpoint-26 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`A80B61BA04C259A055B03537D8354FC9954CA81EA3E198425F2460314B4555EB`.

Headset validation: both weapons, physical movement/turning, and ordinary
gameplay remained correct. Keep checkpoint 27 active.

### Performance audit checkpoint 28 — fire-wrapper probes

The latest shooting log exposed the old shot-construction probe still active
inside the native fire wrapper. For its first 16 records it made three process
memory reads before the redirect, three after it, and three more after Metro's
callee, formatting PRE/REDIRECT/POST vector dumps. For the first three records
it additionally captured and resolved a native stack and followed several
object/vtable pointers solely for logging.

This checkpoint false-gates that diagnostic family. The live weapon lookup,
gameplay ADS refresh, recoil guard, native wrapper invocation, shot redirect,
projectile creation, and all rendering/gameplay behavior remain unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-fire-wrapper-diagnostics-off-20260826-1759/d3d11.dll`,
SHA-256
`25F2E2952EAF5C282E2D5EC20299A745D857950964C7BA4E4171CD8C3BB1F58C`.
The headset-validated checkpoint-27 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`82149064E4488D3BA78607E78B75E1D954D6E2BF52517D15A904A637B4688473`.
The headset-validated checkpoint-24 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`B4C2B2847FC671DA159EF1365CF2AD5CFC2A97D97657F1ADD6C6662AD0B56687`.

Headset validation: the opening hands and ordinary gameplay remained correct.
Keep checkpoint 25 active.

### Performance audit checkpoint 26 — particle-distance census

The preceding runtime log hit the 120-line cap of the retired `VRPose particle:
d=` distance census. That block was added to tune the muzzle-particle radius
and no longer contributes to a functional decision. This checkpoint
false-gates only its counter and formatted messages.

The already-computed object distance, shot and laser gates, weapon-affine
particle fold, corrected matrices, lighter/ambient handling, and all other
rendering/gameplay systems remain unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-particle-distance-census-off-20260826-1651/d3d11.dll`,
SHA-256
`A80B61BA04C259A055B03537D8354FC9954CA81EA3E198425F2460314B4555EB`.
The headset-validated checkpoint-25 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`630B84F0E343033CD0CC4812CBE18259FB942CF59448F5EB464C0D35D138D7BE`.
The exact pre-change DLL is preserved beside it as `d3d11.pre-change.dll`,
SHA-256
`C279D81D23B5760130067935659C44E627CF0FAE0A1018276576C545A4F5D79A`.

Headset validation: ordinary gameplay and rendering remained correct. Keep
checkpoint 23 active.

### Performance audit checkpoint 24 — muzzle-particle gate trace

The preceding short runtime log contained the full 240-line cap of `VRPose
flash particle gate` messages. On each qualifying firing-path object map, that
retired tuning block also performed a shader-map lookup solely to format the
trace. This checkpoint false-gates only that lookup, counter, and message.

The functional shot-window test, proximity calculation, particle-block and
laser classification, weapon-affine fold, live and replay muzzle rendering,
and all other gameplay/rendering systems remain active and unchanged.

Release x64 built with 0 warnings and 0 errors. Candidate archive:
`Builds/performance-flash-particle-gate-trace-off-20260826-1644/d3d11.dll`,
SHA-256
`B4C2B2847FC671DA159EF1365CF2AD5CFC2A97D97657F1ADD6C6662AD0B56687`.
The headset-validated checkpoint-23 DLL is preserved beside it as
`d3d11.pre-change.dll`, SHA-256
`C14D3884FA584173F881D83BBF0ECEF8CA8F844837026D6262B327DA1119D520`.

## Chat work consolidation — 2026-08-26

This section records the work completed during the performance-audit chat so
that the current state and the deferred follow-up work are easy to recover.

### Repository and branch work

- Created and worked on `codex/performance-audit`.
- Synchronized the branch with the then-current hotfix baseline.
- Committed the completed low-risk audit cleanup as `2ad670b` and merged it
  locally into `master` with merge commit `6bded55`.
- Generated captures, probe output, Python caches, and other untracked
  investigation artifacts were intentionally left out of the commits.

### Functional fixes and investigations recorded in this chat

- Preserved and revalidated the projectile-creation redirect used to keep
  shots aligned after physical turning; runtime telemetry showed both
  `redirected=1` and `originRedirected=1`.
- Investigated bullet-hole/decal disappearance and physical-turn shot drift.
  The accepted fix improved the problem substantially, with only the extreme
  top/bottom edge of view still showing occasional decal clipping. Later shot
  drift was traced to tracking/reference translation behavior rather than the
  redirect being absent.
- Recovered the resolution/SSAA behavior after an intermediate rollback caused
  zoomed or corrupted output at values above 1x. The working resolution slider
  fix was restored in the separate resolution work and verified by the user.
- Documented the Survival-versus-Spartan watch difference: restoring saves from
  a Survival test can select the native Survival watch/timer path, while a new
  Spartan game followed by save restoration returns the custom 3D watch timer.
  This was confirmed as a game-mode/save-state issue, not a performance change.
- Investigated muzzle-flash companion geometry and trigger-only stray muzzle
  surfaces. The broad diagnostic work was shelved after testing; the remaining
  per-weapon muzzle companion issue is deferred for a later focused pass.
- Verified the watch, close-range visibility, physical movement/turning,
  shooting, and ordinary gameplay after the accepted audit checkpoints.

### Performance audit completed

The audit progressively disabled diagnostic-only work while leaving functional
rendering, transforms, culling, stereo handling, input, and gameplay paths
active. The completed checkpoints are:

1. retired camera/view, roll, load-library, resolution, UI, hand/watch,
   reticle, single-pass, and related trace families;
2. repeated watch decode/glyph/suppression traces;
3. decal qualification and verification traces;
4. muzzle hunt, flash-particle trace, prologue split-mesh trace, and
   particle-distance census;
5. weapon draw diagnostics and fire-wrapper probes;
6. ADS hand-height trace, LOD census, and flicker census.

Each accepted checkpoint was built as a Release x64 DLL with zero compiler
warnings/errors and was tested in-headset before continuing. The latest active
candidate is the checkpoint-31 flicker-census-disabled build documented above.

### Deferred future project — real single-pass stereo rendering

Runtime telemetry showed `StereoSinglePass::gEnabled=true`, but effectively no
scene draws were folded into one pass (`0 draws/frame folded into one pass`).
The current safe path therefore still uses the twin/double-draw route for the
affected shaders. A real single-pass performance project remains the largest
quality-preserving GPU opportunity identified in this audit, but it is deferred
because it requires auditing and patching roughly 191 vertex-shader variants,
then validating shader constants, stereo transforms, UI, weapons, decals,
particles, and all major levels. It must be treated as a separate opt-in,
checkpointed project and not enabled by simply flipping the existing flag.

## Physical-turn shooting accuracy — accepted 2026-08-27

Extended headset testing confirmed that shot accuracy remains aligned across
physical rotations. The final correction keeps projectile direction on the
completed rendered view basis and maps the tracked 6DOF projectile-origin
offset with the stable body heading published by the camera-render path. This
avoids both transient synthetic values from Metro's `+0x170` camera vector and
the optional motion-aim body anchor, which was unavailable during some
projectile-creation calls.

Final accepted implementation: `e564c12` (`Share rendered body heading with
projectile origin`). Runtime validation showed `redirected=1`,
`originRedirected=1`, and a valid rendered basis across the tested shots. The
player reported accuracy as consistently good during the longer follow-up
test. The temporary shot-basis and projectile-basis loggers were disabled after
acceptance; functional direction and origin redirection remain active.

## VR brightness, watch visibility, and release calibration cleanup — 2026-08-27

This work was completed on `codex/brightness-issue`. It began as an investigation
of why Metro's VR presentation was brighter than the monitor output and why the
game's native brightness control did not affect the submitted eye textures. It
also incorporated two unrelated headset-visible regressions found during the
same test pass: a missing orange/cyan watch child and accidental calibration-mode
activation while opening the VR menu.

### Brightness investigation and accepted presentation paths

- Metro's native brightness control operates before or outside the isolated VR
  final-colour submission path, so it cannot be relied on to grade the eye
  textures. The existing VR-menu brightness slider remains the supported control.
- The isolated high-resolution eye path had been writing sampled linear scene
  values into an 8-bit UNORM target. The accepted corrected path keeps those eye
  images in `R16G16B16A16_FLOAT` through submission and performs the required
  display transfer only when the destination path needs it.
- Added the persisted `brightness_correction` setting and a
  `VR BRIGHTNESS CORRECTION` checkbox immediately below the brightness slider.
  Corrected mode is the default. Disabling it restores the legacy 8-bit output
  format and the original VR-slider gamma mapping; it does not disable the
  slider.
- Corrected mode remaps the slider so the previously useful legacy `0.00`
  appearance is at the `0.50` midpoint. Its gamma is
  `pow(2, (1 - brightness) * 2)`; legacy mode retains
  `pow(2, (0.5 - brightness) * 2)`.
- Several stronger gamma/lift/toe experiments were rejected in-headset. High
  exponents crushed near-black detail into blotchy regions, while shadow lifts
  and aggressive curve changes made the image badly overexposed. The final
  implementation deliberately avoids a grey offset so true black remains black.
- Headset testing found the accepted correction substantially better than the
  original overly bright VR output. Very aggressive darkening can still expose
  limited source shadow detail; the correction toggle is retained for direct
  comparison during future tuning and the planned VR-menu overhaul.

### Complete watch child restored across scene shader variants

- The orange/cyan watch child is a 60-index pass sharing the exact same-frame
  native instance matrix as the 6108-index watch casing, but its material shader
  can vary by scene. Requiring the previously observed shader pair caused that
  part of the watch to disappear in another area.
- The route now recognizes the 60-index candidate without scene-specific shader
  hashes. `SubstituteLeftHandCompletedInstance` still requires exact same-frame
  native-matrix equality with the casing, which remains the identity/safety
  discriminator and prevents unrelated 60-index geometry from being reparented.
- Headset validation confirmed the missing orange and cyan portions returned.

### Release input cleanup

- All runtime calibration hotkeys except weapon calibration are disabled. The
  journal, left hand, watch assembly, watch time, lighter body, and lighter flame
  modes are forced off and their activation chords are gated. Weapon calibration
  remains available and unchanged.
- This prevents the VR-menu thumbstick chord from accidentally leaving the watch
  or another prop in adjustment mode. Existing loaders and saved calibration
  formats remain in place for compatibility and for a future explicit menu-based
  calibration UI.

### Right-hand calibration recovery and embedded clean-install default

- Disabling the unrelated hotkeys exposed that the non-combat right hand had two
  distinct persistence layers. `vr_right_hand_pose.bin` (`RHP1`, 3,860 bytes)
  stores only the frozen 3,856-byte bone/finger palette; it does not store the
  rigid hand position or orientation. That file was never modified during this
  work. Its verified SHA-256 was
  `F2B29B79160DB15990510039279ABA0BFC84BE767150DF5E7B1B7E733E9ABCC3`.
- The rigid six-value calibration was preserved in
  `vr_right_hand_offset.txt`, but the committed right-hand implementation had
  stopped loading it and used older embedded constants. Loading the saved file
  restored the user's calibrated hand in headset testing.
- A valid `vr_right_hand_offset.txt` is now loaded once per process as an
  optional user/controller override. Values must contain six finite floats;
  position components are limited to +/-0.75 and rotation components to +/-1.5
  radians. Missing, malformed, or implausible files safely retain the built-in
  defaults.
- The headset-validated position `(0.090000, -0.191999, 0.002000)` and rotation
  `(-0.940000, -1.039998, -0.519999)` are embedded as the clean-install defaults.
  They therefore apply across levels, save files, new campaigns, and installs
  where the optional override file is absent.
- Two intermediate orientation approaches were rejected and are not present in
  the final source: persisting a startup-relative anchor required an awkward
  physical pose, while replacing the calibrated relative transform with raw
  absolute controller orientation lost the established model-to-controller
  basis. The temporary `vr_right_hand_anchor.bin` produced during that test is
  ignored by the final implementation.
- Release caveat: OpenVR controller coordinate systems can differ by hardware.
  The embedded values provide a validated default, but a future VR-menu overhaul
  should expose intentional right-hand position/rotation calibration and save it
  to `vr_right_hand_offset.txt`; hidden calibration chords should remain disabled.

### Diagnostics and final verification

- The temporary four-message submission-colour-space trace used during the
  brightness investigation was removed before commit. No chat-specific runtime
  census, probe, capture hook, or generated diagnostic artifact was added to the
  tracked tree. The one-time successful right-hand configuration-load message is
  normal persistent-setting telemetry rather than a diagnostic loop.
- Release x64 built with 0 warnings and 0 errors after the cleanup. Final DLL
  SHA-256 before merge/deployment:
  `5963E169DD8F417702D13F2D34E955C339D3D85C441E0082F7677723319E2C1D`.

## Survival analog watch left-hand attachment — 2026-08-29

This focused pass moved the Survival-mode analog watch to the same calibrated
left-wrist location as the Spartan digital watch. The completed work is on
branch `codex/survival-watch` and was validated in-headset before merge.

### Native Survival watch identification

- Survival gameplay uses the 2,538-index analog casing as the exact physical
  anchor. Its shader identity is VS `65C62A5148831C58` plus PS
  `BD723CC8D6496169`.
- The observed analog assembly also contains 288, 1,782, 24, 6, 252, 174,
  54, and conditional 60-index passes. Counts alone are not sufficient because
  the game reuses them for unrelated viewmodel geometry; the same-frame native
  anchor/pivot relationship is retained as the safety discriminator.
- The Survival casing and static face pieces are rebuilt as one rigid assembly
  under the completed left hand. This avoids the native right-hand viewmodel
  lever arm that previously made the casing and face swing apart.
- The two live clock-hand meshes retain their native relative rotation, but the
  relative rotation is conjugated around the measured dial center
  `(-0.000366, 0.010500, -0.000183)` rather than the model origin. This keeps
  the hands seated while preserving analog time movement.
- The private single-instance replacement buffer is drawn from instance zero;
  this is required because Survival's pooled source draws can use non-zero
  source instance locations.

### Gas-mask timer hand

- The red gas-mask timer is the six-index pass with VS
  `9420C1D720E7C3B8` and PS `790E474FEEAF6880`. The 18-index pass that shared
  the red material was tested and rejected as unrelated geometry.
- The red pass is submitted continuously, but its render order changes: during
  watch/filter inspection it follows the casing anchor, while ordinary masked
  gameplay can submit it first. The old same-frame anchor requirement therefore
  made it appear only during the inspection animation.
- The final route accepts the exact red pass when the immediately previous
  native anchor is available, uses the current completed left-watch base, and
  caches the last validated timer-hand local angle for the early draw order.
  Textures, alpha parameters, predication, and native shader visibility state
  remain unchanged.

### Mode and lifecycle gates

- During the opening/prologue sequence the Survival watch must remain hidden,
  matching the Spartan watch behavior. The Survival route therefore requires
  `VRPose::IsGameplayModeActive()`.
- After the prologue, the route continues using the ordinary completed
  left-hand gameplay parent. This fixes the previous fallback to the native
  right-hand watch after the prologue without reopening the intro leak.
- Separate prologue and normal-gameplay watch calibrations remain intact:
  `vr_prologue_watch_assembly_offset.txt` for the prologue and
  `vr_watch_assembly_offset.txt` for normal gameplay.

### Validation and cleanup

- User headset validation confirmed: casing/face alignment, live analog hands,
  persistent red gas-mask hand, no watch before the main menu, and continued
  left-hand attachment after the prologue.
- Release x64 builds compiled successfully; the final deployed DLL for this
  pass was SHA-256
  `F6125D90FC544B0A3D840F3B79ED3EAF55DC9C65D4CEB7B183533B33C8868EC3`.
- Temporary Survival-watch census, timer-matrix, timer-visibility logs, and
  video-analysis artifacts from this pass were removed after diagnosis. No
  temporary diagnostic switches remain enabled in the source.

## Basic left-grip gestures — 2026-08-30

This pass established the basic gesture foundation on branch `codex/gestures`.
The user is intentionally deferring advanced held-equipment visuals until the
remaining basic gestures have been implemented.

### Two-hand weapon contact gate

- Left grip only engages two-handed weapon steering when the grip press begins
  while the left controller is inside a narrow weapon-local rectangular contact
  volume. Holding grip outside the box and moving into it cannot acquire the
  weapon; the player must release and begin a new press while touching it.
- The box is long enough to reach the front grip but deliberately thin around
  the weapon's sides. Its rear edge begins eight centimetres forward of the
  right-controller grip, so the stock/rear cannot consume left grip during body
  gestures.
- Once acquired, the contact box is no longer consulted. Two-hand steering stays
  latched while left grip remains held, even if the support hand moves outside
  the box; release, invalid tracking, weapon loss, or recentering clears it.
- User headset testing accepted the current dimensions, front-only acquisition,
  press-edge requirement, and held-grip latch as the working baseline.

### Native flashlight gesture

- Pressing left grip beside the left temple toggles the flashlight through the
  resolved native torch object and its enabled-state virtual method. It does not
  synthesize keyboard or controller input and therefore does not introduce an
  extra prompt-source switch.
- The flashlight volume is headset-local and requires a distinct negative-X
  (left-side) offset. A grip directly in front of the face no longer toggles the
  flashlight, leaving that space for gas-mask removal.
- User validation confirmed the flashlight triggers only at the left temple.

### Basic gas-mask gestures

- Holding left grip at the left hip arms an equip gesture. Moving the held hand
  to the face invokes Metro's native `cplayer:action_gasmask`; releasing before
  reaching the face cancels without invoking the action.
- While the mask is worn, pressing left grip near the face and pulling the hand
  beyond the removal threshold invokes the same native action to remove it.
- State is read from the same active equipment slot used by Metro's dedicated
  gas-mask action handler at `metro.exe+0x4F9F30`: player `+0xB70` resolves the
  equipment service, `+0x6A0` selects the active slot, and slot `+0x118` is the
  authoritative state byte. Stable `0xFF` means off and stable `0x01` means
  worn; other/transitional values are treated as unknown and gestures refuse to
  guess. The old player `+0xAE0` proxy and checkpoint fallback were disproven
  across levels and are no longer used.
- User headset validation confirmed equip/removal in ordinary fresh-level play,
  across another level/save, and removal immediately after loading a checkpoint
  already saved with the mask worn.

### Native gas-mask filter gesture

- With the mask authoritatively worn, pressing and holding left grip at the
  chest, carrying the hand to the mouth, and twisting at least 20 degrees queues
  a direct native filter replacement. Releasing before completion cancels it.
- The broad gas-mask-removal head sphere overlapped the upper-chest grab,
  especially while seated. Gesture routing now reserves presses in the explicit
  chest volume for the filter and requires mask removal to begin at the face
  outside that chest volume.
- Filter ownership is resolved from the live equipment service through its type
  lookup and filter-equipment virtual boundary. The gesture invokes Metro's
  native replacement at `metro.exe+0x3415E0` without synthesising keyboard or
  controller input or opening the equipment menu.
- User validation confirmed the timer resets. The successful trace recorded
  chest and mouth recognition, a 22.1-degree twist, native result `1`, and filter
  capacity changing from `98.80/300.00` to `300.00/300.00`. Metro did not play
  the normal equipment-menu animation, which is accepted for the basic gesture.
- Showing a filter model in the left hand is deferred to the advanced-equipment
  visual pass for the same rendering/animation ownership reasons as the held
  gas-mask model.

### Deferred advanced mask presentation

- Native transition-draw replay was tested as a way to show a mask held in the
  left hand. Replaying the captured passes produced malformed repeated geometry
  and persistent render corruption; target rebinding did not make replay safe.
- The captured 9,720/120/216/216 and later 720/5,760/402 meshes are transition
  props, not a stable persistent worn-mask model. Stable render-census attempts
  also did not expose a safe persistent model identity.
- Advanced held-mask visuals are therefore deferred. Target-isolated replay,
  stable visual census, transition/class/queue probes, and late-assembly hiding
  are all disabled in the current source. The reversible memory-snapshot probe
  used to find the native state byte is also disabled in the clean baseline.

### Native charger gesture

- The charger uses Metro's direct `cplayer:action_charger` method at
  `metro.exe+0x288EE0`; it does not synthesize D-pad input or require the
  equipment menu to be open. The normal right trigger continues to operate the
  charger after it is drawn.
- Charger state is read from the same active equipment slot used by Metro's
  dedicated action-0x4C handler at `metro.exe+0x4FA730`: player `+0xB70`
  resolves the equipment service, `+0x6A0` selects the active slot, and slot
  `+0x11A` is the authoritative state byte. `0xFF` means put away; every other
  value follows Metro's native drawn/active branch.
- When put away, left grip must begin inside the narrow central front-waist
  volume and remain held through a deliberate pull of at least 18 centimetres,
  including at least 10 centimetres upward or outward. Releasing before the
  threshold cancels without drawing it. When already out, a new left-grip click
  in the same volume immediately puts it away.
- The central charger volume ends at body-local X `-0.12 m`; the gas-mask hip
  volume begins at `-0.18 m`, leaving a deliberate six-centimetre gap. User
  headset validation confirmed charger draw/holster, early-release semantics,
  and that the left-hip gas-mask gesture remains available.
- Chest and waist acquisition use an HMD-position-relative, yaw-only body
  frame. Looking up/down or tilting the headset cannot move these body zones;
  headset-relative face zones and weapon-relative contact zones intentionally
  continue to follow their corresponding physical targets.
- Attaching the persistent charger model to the left hand is the next advanced
  presentation step; the validated basic interaction currently retains Metro's
  native right-hand presentation.

### Current validated build

- Release x64 compiled successfully and was deployed to the Metro game
  directory. DLL SHA-256:
  `874CF931FCAF96588B63EA397843A2C74E36084FEEFA0F1FAB880C3533616859`.
- F12 remains reserved by the game and must not be used for diagnostics.

### Pneumatic direct-path correction — 2026-08-31

- Offline analysis initially subtracted a rounded module base from pointers in
  `metro_dumped.bin`. The dump's actual image base is `0x7FF7A7F70000`; the
  previous assumption shifted pneumatic method RVAs by `0x90000` and produced
  unrelated disassembly.
- A second adjacency assumption was also disproven. The actual
  `weapon_pneumo` primary vtable is at `metro.exe+0xAE4690`, its live
  `update_ssss` routine starts at `+0x2CCCB0`, and the separately named
  `switch2_pump_in/out` routines are pump-stroke animation callbacks rather
  than the command that enters pump mode.
- The action-13 equipment listener route was disproven: it only updates the
  equipment subscription/event plumbing and does not put the gun into its pump
  presentation. The hold-X path no longer invokes that listener.
- The real pump-mode entry routine is `metro.exe+0x2CCA20`. It checks the
  weapon's native can-pump virtual and, when allowed, moves the internal pump
  state machine at weapon `+0x700` to state `0x43`. A guarded hook on the real
  `update_ssss` captures the concrete live pneumatic weapon object. Hold X
  validates capture freshness and the exact vtable, then directly invokes the
  mode-entry routine. Hook installation retries while Metro's protected page
  remains encrypted instead of permanently failing during early startup.
- Headset testing showed that optional `update_ssss` is not executed in the
  tested weapon state, so callback-based capture remained null. The fallback
  resolver now searches committed private object memory for the exact
  `weapon_pneumo` primary vtable plus all four constructor-proven embedded
  state-machine vtables (`+0x430`, `+0x4F0`, `+0x5A0`, and `+0x700`). It then
  accepts only an instance whose native `can_pump` virtual returns true.
- To avoid depending on the still-protected `+0x2CCA20` page, hold X performs
  that routine's complete native body through the same virtual boundaries:
  call `can_pump`, then transition the embedded pump state machine at `+0x700`
  to state `0x43` with the immediate flag set.
- Correction after headset validation: the private-memory fallback above was
  both incorrect and responsible for a visible split-second freeze on hold X.
  It searched for the base `weapon_pneumo` vtable, but every concrete pneumatic
  constructor replaces that vtable; the runtime log consequently reported zero
  matches. The whole-process memory scan has been removed.
- Offline constructor analysis identified all four concrete descendants and
  their primary vtables. The replacement hooks the common `weapon_pneumo`
  constructor and stores its results in a fixed 32-entry registry. Hold X checks
  only the recent registry entries (plus existing live-weapon captures), accepts
  only a constructor-proven pneumatic vtable, calls `can_pump`, and performs the
  state-`0x43` transition. This path is bounded and cannot cause the old scan
  hitch. The next headset test must establish whether the protected constructor
  hook becomes available early enough to capture the equipped weapon.
- Candidate checkpoint:
  `Builds/pneumatic-constructor-registry-state43-20260831`. Release x64 built
  with zero errors and deployed with matching SHA-256
  `CA00C93068423CD1777783B6933A1E51405F99BFED749549F92E07B0FA6FFDD3`.
- The X-hold design was subsequently retired at the user's request. Metro also
  uses held X to pick up replacement weapons, and delaying/suppressing the
  virtual X button prevented that native interaction. X is once again forwarded
  continuously from physical press through release, restoring Metro's original
  reload/use/pick-up semantics. The pneumatic hold counter, delayed short-press
  pulse, queue trigger, and all pneumatic hook installation calls are disabled.
  Pneumatic mode work is paused pending selection of a non-conflicting gesture.
- Restored-X checkpoint:
  `Builds/pneumatic-x-hold-disabled-native-x-restored-20260831`, deployed
  SHA-256 `71B5216E55CA7A16BBB87DBC61750EF34C43EA9B7EABB1E8F218D0AD689878AB`.

## Per-weapon corrected grip pivot — 2026-08-31

- Video `2026-08-31 20-39-54.mp4` showed an assault rifle moving farther away
  as the controller rotated. This reproduced the earlier remote-pivot symptom,
  but the remaining lever arm came from the rifle's saved position profile.
- The renderer previously translated a weapon by its saved controller-space
  position adjustment while continuing to rotate around the inherited SMG or
  revolver base grip. For the rifle shown, the active profile's depth correction
  is `+0.162 m`, leaving the visual grip roughly 16 cm from its pivot.
- Normal weapons now use `effectiveGrip = baseGrip - savedPositionAdjust` as
  their rotation pivot. The existing translation is algebraically
  `controller - effectiveGrip`, so the calibrated resting position is unchanged
  while controller rotation occurs around the corrected grip. Charger and
  independent-hand transforms are explicitly excluded, as are aim and shot
  direction.
- Candidate checkpoint: `Builds/assault-rifle-corrected-grip-pivot-20260831`.
  Release x64 built successfully and was deployed with matching SHA-256
  `D2F31347C1F3C32A2584B4984A135BD691AA8AE3A4A89CE860C0EAF90D16DFE2`.

## Advanced charger presentation — 2026-08-31

- A stable automatic out/away census isolated the complete six-pass charger
  assembly: the 14,544-index skinned body plus five rigid 99/180/84/180/180
  gauge/detail passes. Suppressing all six removed the charger and nothing
  else in headset testing.
- The main skinned body's weighted visible centre is evaluated from its live
  bone palette and pinned to the submitted left-wrist landmark. This avoids
  rotating around Metro's remote native instance origin, which previously made
  the charger swing around the hand.
- Each rigid gauge layer measures its own indexed geometry centre. Its native
  body-relative offset is retained under a rotation-only parent extracted from
  the corrected charger transform. This removes the inherited hand-solve
  scale/shear that expanded the natural 12.1-centimetre gauge offset to roughly
  29–50 centimetres. Headset testing confirmed the gauge is reassembled and
  rotates correctly with the body.
- The gauge/detail layers are parented to the final calibrated body centre, not
  the uncalibrated wrist pivot. Whole-assembly position and rotation therefore
  move all six passes together.
- The right-hand islands use a charger-specific controller transform while the
  exact charger renderer is active. They preserve Metro's live charger hand
  animation but consume only the charger right-hand position/rotation profile.
  Normal guns never read either charger profile.
- Final headset-verified clean-install defaults are embedded:
  - charger assembly position `0.199767 0.098039 0.140492`, rotation
    `0.546974 -0.192550 0.000000` radians;
  - charger right-hand position `0.071883 -0.165728 0.000000`, rotation
    `-0.058000 0.000000 0.000000` radians.
  Optional `vr_charger_offset.txt` and `vr_charger_right_hand_offset.txt` files
  may override those defaults.
- The temporary charger calibration chords are release-disabled after the user
  calibrated and verified the result across levels and a different save.
  Weapon calibration is also explicitly ineligible while the charger visual is
  active, preventing LT+left-grip from selecting or saving a gun profile.
- Before and after charger calibration, `vr_weapon_calibrations.txt` retained
  SHA-256 `79B188F310065E928E31327205F41E65C44F4CE85DB5113FB8A724F8179E81D6`
  and `vr_right_hand_offset.txt` retained SHA-256
  `D20849FAB1F70657C858F1A0943980E8F4E3180DA307213537BC2EE5B4F3E602`.
  This confirms neither protected weapon placement file was modified.
- Charger route/geometry traces and temporary detail-shader dump hooks are
  disabled in the release build. The runtime centre measurements required for
  skinning and rigid assembly remain active without periodic diagnostic logs.

### Current validated charger build

- Checkpoint: `Builds/charger-left-hand-final-calibrated-20260831`.
- Release DLL SHA-256:
  `F3449FEBF9436D07EEE7E7B9D7302BEEE9F902470016AE810A3392526D37B40D`.

## Gestures branch consolidated handoff — 2026-08-31

### Release design rules

- Working equipment gestures call Metro's native gameplay methods or native
  equipment objects directly. They do not synthesize keyboard keys, controller
  D-pad presses, or temporary equipment-menu input. This avoids unnecessary
  keyboard/controller prompt switching and keeps the actions independent of
  whether the equipment radial is visible.
- Gesture acquisition is press-edge based. A left-grip press is assigned to one
  eligible interaction at its starting location; moving into another zone while
  the same press remains held cannot steal or create an interaction.
- Body equipment zones use a yaw-only body frame derived from the HMD position.
  They stay at the waist/chest when the player looks down, pitches the headset,
  sits, or stands. Face and temple targets remain headset-local because they
  represent physical points on the head. Weapon contact remains weapon-local.
- Gesture routing priority is charger, gas mask, filter, then flashlight. The
  charger and gas-mask acquisition volumes have a deliberate dead gap, and the
  chest filter volume is excluded from face-removal acquisition. This prevents
  one left-grip press from invoking two pieces of equipment.

### Confirmed interaction map

- Two-hand weapon steering: begin left grip inside the thin front-only
  weapon-local rectangular contact box. The rear/stock is excluded. Once
  acquired, steering remains latched until grip release and no longer checks
  distance, preventing the support hand from dropping the gun during motion.
- Flashlight: press left grip beside the left temple. Only the left side is
  accepted; a hand in front of the face belongs to gas-mask removal. The action
  resolves the current native torch object and toggles its actual enabled state,
  which was validated across levels and saves.
- Gas-mask equip: begin at the left hip, hold, and carry the hand to the face.
  Releasing early cancels. Gas-mask removal: when authoritatively worn, begin at
  the face and pull away while holding grip. Both directions invoke Metro's
  native `cplayer:action_gasmask` transition.
- Filter replacement: while the mask is worn, begin at the chest, carry the
  hand to the mouth, and twist at least 20 degrees. The native replacement
  function at `metro.exe+0x3415E0` resets the real filter timer. A held filter
  model and menu animation are intentionally deferred; timer behavior is the
  validated basic interaction.
- Charger draw: begin in the narrow central front-waist volume and pull at least
  18 cm, with at least 10 cm upward or outward. Releasing early cancels. Charger
  holster: click the same waist volume while it is already out. The native
  `cplayer:action_charger` method at `metro.exe+0x288EE0` owns gameplay state,
  and normal right trigger use remains unchanged.
- Reload/use/pickup: physical X is forwarded continuously for the full press.
  A short press reloads and a held press can pick up a replacement weapon.
  Pneumatic pump mode is not assigned to X and is currently deferred.

### Authoritative equipment state

- Gas-mask state is read from the active equipment service rather than inferred
  from the last gesture, level defaults, animations, or a checkpoint heuristic.
  Player `+0xB70` resolves the equipment service, `+0x6A0` selects the active
  slot, and gas-mask slot `+0x118` reports stable off (`0xFF`) or worn (`0x01`).
  Transitional/unknown values are never guessed.
- Charger state uses the same service and selector with slot `+0x11A`.
  `0xFF` is holstered; every other value follows Metro's native active branch.
- Player and equipment objects are resolved again when they change, so level,
  save, checkpoint, and player-object transitions do not rely on stale pointers.

### Charger presentation

- The complete charger is six render submissions: one 14,544-index skinned body
  and five rigid gauge/detail submissions. The body is pinned to the left wrist
  using its live skinned visible centre. Rigid details retain their measured
  body-relative offsets under a rotation-only parent, keeping the gauge attached
  without inheriting scale/shear.
- The assembled charger uses its own left-hand calibration and its own
  right-hand calibration. Normal weapons do not consume either profile, and
  charger calibration cannot write `vr_weapon_calibrations.txt` or the ordinary
  right-hand offset.
- Embedded headset-validated defaults are position
  `0.199767 0.098039 0.140492`, rotation
  `0.546974 -0.192550 0.000000` radians; right-hand position
  `0.071883 -0.165728 0.000000`, rotation
  `-0.058000 0.000000 0.000000` radians.

### Weapon behavior retained by this branch

- Saved weapon position adjustments now also correct the effective rotation
  grip: `effectiveGrip = baseGrip - savedPositionAdjust`. Resting calibration is
  unchanged, but weapons with large offsets no longer orbit or move away when
  rotated. The assault rifle in `2026-08-31 20-39-54.mp4` was headset-validated
  after this correction.
- The charger/right-hand special transforms are excluded from that weapon fix.
  Reticle direction and projectile direction are also unchanged.

### Deferred or rejected work

- Persistent held gas-mask and filter models are deferred. Replaying transition
  draws produced malformed repeated geometry and persistent render corruption;
  the captured meshes were animation props, not safe persistent objects.
- Pneumatic weapon pumping is deferred until a non-conflicting gesture is
  chosen. Holding X is not available because Metro reserves it for weapon
  pickup. The failed heap resolver was removed after it caused a visible hitch,
  and no pneumatic hook or action is installed by the release path.

### Release diagnostic cleanup

- Removed the active equipment action/caller trace, gas-mask code and vtable
  dumps, automatic post-action memory snapshots, queue guard-page traces,
  translated pneumatic replay/trace polling, and temporary input-device/window
  scans from the controller update path.
- Native filter and gas-mask hooks remain only because the working gestures use
  their trampolines/owners. Their per-action argument, caller, before/after, and
  object-relation logging was removed; only bounded installation/failure and
  gesture-transition messages remain.
- Disabled periodic ADS instance logging, periodic weapon calibration logging,
  one-shot weapon geometry readbacks, and the muzzle-flash transform trace. The
  geometry readbacks performed GPU staging/map stalls and are not required once
  the weapon and charger pivots have been established.
- F12 is reserved by Metro and must never be assigned to diagnostics.
- Release-clean checkpoint:
  `Builds/gestures-release-diagnostics-cleanup-20260831`. Release x64 compiled
  successfully and was deployed with matching SHA-256
  `5CF0AB536C7B4B602C6F2181D94D7C7BA96784A4780798A783EA85DE0B354CB6`.

## Persistent weapon calibration identities — 2026-09-01

- Symptom: on Ghosts, Shambler and Kalash initially loaded at their calibrated
  positions, but both moved backward to the same wrong position after cycling
  through the revolver. Reloading the chapter in the same Metro process then
  reproduced the wrong positions immediately. The revolver remained correct.
- Weapon offsets are external to Metro's per-level saves in
  `vr_weapon_calibrations.txt`. Only the Bastard SMG and revolver previously had
  explicit base-weapon keys; every other profile depended on a level- and
  attachment-sensitive complete-mesh similarity score.
- Calibration records are now version `v4`. They retain the complete mesh set
  for compatibility and also store a stable body draw plus a deterministic
  synthetic weapon key. New weapons register that identity when their normal
  LT + left-grip calibration is saved.
- Existing unkeyed `v3` profiles migrate without reacquiring the weapon. The
  migration infers the substantial body draw already present in each saved
  mesh set, assigns its synthetic key, and rewrites the records as `v4`.
- A confirmed native or registered body key is authoritative and cannot be
  displaced by mesh-similarity hysteresis. The key also remains latched across
  the incomplete hand/watch-only frames submitted during weapon swaps.
- Final root cause of the identical backward shift: the renderer's static
  `sCurrentWeaponIndex` changed to the revolver grip when the revolver body was
  seen. Registered Shambler/Kalash bodies did not reset that separate renderer
  state, so both inherited the revolver base grip even while their correct
  saved offset profiles were selected. The grip difference contributed the
  same approximately 24.6-centimetre depth shift to both weapons and persisted
  across chapter loads within the process.
- Registered weapons now explicitly restore the canonical non-revolver base
  grip when their body draw identifies them. Headset validation on Ghosts:
  Shambler, Kalash and revolver all retained their correct calibrated positions
  through repeated full weapon cycling.
- Investigation-only identity migration/switch diagnostics were removed before
  release. No periodic logging, draw readback, trace, or probe was added.
- Release x64 compiled successfully and was deployed with matching SHA-256
  `142EABFE5DF6D17E2EE411F707E849383A3C3A81B963A9C15710B9F16D2DE6E7`.

## NPC projectile aim ownership fix — 2026-09-01

- User video confirmed that enemy rounds were traveling toward the player's
  current VR weapon aim while the player was not firing.
- Root cause: the shared projectile-creation hook at `metro.exe+0x3D7FF0`
  rewrote every projectile whenever the VR barrel direction was available;
  it had no player-fire eligibility gate.
- Fix: require `IsFiring()` before applying the VR barrel direction and
  translated VR origin. NPC projectiles now retain Metro's native direction
  and origin, while player projectiles continue using the VR redirect.
- Release x64 compiled with 0 warnings and 0 errors, was deployed, and was
  headset-validated by confirming NPC fire no longer follows the player's
  aim while the player is not shooting.

## Clothing-change and town hand attachment — 2026-09-01

- Armory's clothing change replaces the ordinary 12,132-index combined-hand
  mesh with a 21,762-index arms/hands mesh. The replacement is identified by
  its exact index count and VS/PS hashes, excluded from weapon-body identity,
  split into left/right hand-and-cuff runs, and routed through the established
  controller attachment paths while sleeve-only runs are omitted. Headset
  testing confirmed the post-change gun/right hand follows the right controller
  and weapon calibration, while the left hand/watch and digital display follow
  the left controller.
- The weaponless town uses the separate 21,252-index prologue arms mesh. Its
  right glove is now attached through the independent right-controller path
  instead of being classified as a weapon body. A captured live palm landmark
  supplies the controller pivot.
- Follow-up video showed that the prologue glove was positioned correctly but
  rendered roughly twice its anatomical size. This mesh is already authored at
  full hand size; unlike the rigid left-hand path, it was retaining the 2x
  weapon-model stereo compensation. Only the 21,252-index independent right
  hand now uses a 1x instance scale. Normal weapon calibration, the ordinary
  12,132 hands, and the post-clothing combat mesh retain their existing scale.
- Candidate checkpoint:
  `Builds/prologue-town-right-hand-anatomical-scale-20260901-1842`. Release x64
  compiled with 0 warnings and 0 errors and was deployed with SHA-256
  `0B64DE80E6F1E4C2C2B59C390A7EFE5A96F5D8F312419B958B6139577A131596`.
- Follow-up comparison with the working 12,132-index normal-town route found
  that it freezes and binds a stable right-hand bone palette before both palm
  evaluation and drawing, while the new 21,252 route retained Metro's live
  scripted right-arm palette. The prologue mesh now receives the same stable
  treatment in both eyes using a separate skeleton-specific cache,
  `vr_prologue_right_hand_pose.bin`; the incompatible normal-game pose file is
  neither read nor modified. Candidate checkpoint:
  `Builds/prologue-town-right-hand-frozen-palette-20260901-1849`, deployed
  SHA-256
  `7FFAC624D7B8CE893D89F4A1B5EF954BAFC1C2ABB63D003CD2004B8A19409BAC`.
- **Correction after headset rejection:** the shared 21,252 identity is not a
  sufficient opening-sequence state test. Metro also submits that asset in this
  town, while the earlier clothing-transition probe proves a normal 12,132
  combined hand with the post-change material is submitted there as well. The
  two attempts above incorrectly promoted the reused opening arm into an extra
  town right hand; scale and palette changes could not make that the intended
  asset. Those right-hand changes were removed, and the temporary generated
  `vr_prologue_right_hand_pose.bin` was removed from the game directory (a
  recoverable copy remains in the rejected-build archive).
- The normal 12,132 town hand is now authoritative whenever it was observed in
  the current or preceding two frames. In that state the complete reused
  21,252 draw is suppressed, preventing both its wrong right hand and duplicate
  left/watch parenting. Actual opening-sequence behavior remains on its prior
  path when no normal town hand is active. Candidate checkpoint:
  `Builds/town-normal-hand-authority-20260901-1857`, deployed SHA-256
  `BA27785ABB54FCAEDCA37529E51E07044383068CD545692BD4C39441F4012AEA`.
- The first authority build did not activate in headset testing. The preserved
  clothing-transition probe explains why: the intended hidden 12,132 hand is
  submitted with `pass=0`, while the marker incorrectly required
  `IsViewmodelPass()`. The established 12,132 transform route itself has always
  accepted that pass-independent submission. The marker now uses the identical
  count/instance-layout criteria, so this town follows the same hidden-hand
  mechanism as other weaponless towns and suppresses the malformed off-screen
  21,252 arm. One-shot log records confirm both ownership and suppression
  without periodic output. Candidate checkpoint:
  `Builds/town-hidden-hand-pass-independent-20260901-1902`; Release x64 built
  with 0 warnings and 0 errors and was deployed with SHA-256
  `2FD253309E476FD019E65912050670A244A73461DD31CE7E09BE4EF65E5CCB98`.
- **Correction after the second headset rejection:** the 12,132 marker did
  activate and the reused 21,252 draw was suppressed, but the hand remained
  malformed. The same live session also submitted the exact clothing-specific
  21,762 hands mesh. Therefore this town does not use the ordinary hidden hand
  as its final authority; it uses the already-present hidden 21,762 hand, while
  the 12,132 and 21,252 assets are adjacent legacy/scripted submissions.
- Offline skinning of the captured 21,762 right-hand geometry proved the
  deformation source. Applying its captured compatible palette produced a
  20.5 cm depth span; applying the frozen 12,132 town-hand palette expanded it
  to 48.5 cm. The first shared rows match, but clothing-specific wrist/finger
  rows 99 through 117 differ materially.
- The new candidate makes 21,762 authoritative when recently observed,
  suppresses both 12,132 and reused 21,252 submissions in that state, and
  supplies the seven captured clothing-compatible right-hand rows only in the
  weaponless right-hand path. Combat does not use that frozen palette, so the
  validated gun/right-hand weapon calibration route is unchanged. Release x64
  compiled successfully with 0 errors. Candidate checkpoint:
  `Builds/town-post-clothing-hand-authority-20260901-1913`; deployed with a
  matching installed hash. Headset testing from the same town save confirmed
  the forced right hand now renders normally, SHA-256
  `6F2D6D395050B943BB6740E66561D998197604CD87F03D042B11C2594A9D1F9D`.
- Release cleanup removed the temporary one-shot ownership/suppression logs;
  no clothing-change draw probe, capture, or periodic diagnostic remains.
- The complete headset-validated 3,856-byte normal right-hand bone palette is
  now embedded in the DLL as exact IEEE-754 bit patterns. This also supplies
  the common base for the seven embedded clothing-specific row corrections, so
  clean installs no longer need to capture or create
  `vr_right_hand_pose.bin`. A valid existing `RHP1` file remains supported as
  an optional user override. The embedded payload is byte-identical to the
  previously validated file with SHA-256
  `F2B29B79160DB15990510039279ABA0BFC84BE767150DF5E7B1B7E733E9ABCC3`.
- Release-clean checkpoint:
  `Builds/town-post-clothing-hand-release-clean-20260901-1935`. Release x64
  compiled with 0 warnings and 0 errors and was deployed with matching SHA-256
  `13B423835C2092A50123F2484F76B15E798A64D18FCFE4AC0245DDC07AB88478`.
  Its functional hand routing and palette data are identical to the
  headset-validated candidate; only diagnostic removal and embedding of the
  byte-identical clean-install pose default followed validation.

### 5.21 Scope completion checkpoint — 2026-09-01

- Added the first identified 4× optic configuration to the attachment-driven
  scope matcher. Its captured complete mesh set is `12 27 42 60 144 273 288
  360 2517 4647 10662 11751 15222 21762`.
- The 4× path uses the same native Metro visual ADS trigger and stable VR eye
  projection handling as the confirmed 2× optics, with a fixed `3.488x`
  requested magnification. The existing 2× values and signatures are
  unchanged.
- Headset validation confirmed the known 2× optics and the new 4× optic work
  correctly. Unknown, iron, reflex, and unrecognized configurations remain
  excluded from visual ADS.
- Tightened the gun-to-eye gesture hysteresis so ADS/zoom engages at `0.12 m`
  vertical hand-to-HMD separation and releases at `0.18 m`. The values are
  compile-time constants for now; a future VR-menu control can expose the
  engage threshold while deriving the release threshold from the hysteresis
  margin.
- Scope/ADS changes were built Release x64 with 0 warnings and 0 errors and
  deployed while Metro was closed. Installed DLL SHA-256:
  `F454447925700F952AF61BB4D927BFA5BF12FB71B507F6254D50D63ED50C1716`.

### 5.22 Helsing physical-arrow barrel fix — 2026-09-03

- Helsing differs from the already-correct firearms because it creates a
  persistent physical `HELSING_ARROW` object, parks it at the player body root
  during its delayed firing animation, and later creates/applies physics in a
  separate arrow-specific release operation. Redirecting the shared projectile
  wrapper or rewriting the arrow after movement begins is too early or too late
  respectively.
- The concrete arrow vtable is `metro.exe+0xA0EB60`. The lightweight hook at
  `metro.exe+0x37D5B0` identifies only arrows parked on the local player's body
  axis and records their pointer. The ownership test accepts Metro's observed
  standing and lower-stance camera/root separations (`0.75..2.25 m`) while
  requiring less than `0.35 m` horizontal separation. It does not depend on the
  transient trigger state, which can clear before the delayed release.
- The arrow-specific release at `metro.exe+0x37D730` creates the live physics
  object and applies Metro's head-facing velocity. After the native release,
  tracked local arrows only are moved through the same physics position setter
  Metro uses (`vtable+0x58`) and receive barrel-facing velocity through its
  velocity setter (`vtable+0x40`). The native projectile-speed scalar is
  preserved. The arrow transform/origin/direction bookkeeping fields are kept
  consistent with the corrected physics state.
- Helsing origin now uses the same controller-relative-to-head world conversion
  as the headset-validated firearm muzzle path. The earlier cached viewmodel
  translation was only about five centimetres from the HMD and therefore still
  appeared to launch arrows from the player's head.
- NPC isolation is structural: NPC arrows are never entered into the local
  pointer registry unless created virtually on the player's exact horizontal
  body axis while the local Helsing is active. The release hook consumes the
  registry entry once, so unrelated and later NPC physics operations remain
  native.
- Headset validation confirmed all arrows originate with the gun and travel
  where its barrel points. Video `2026-09-02 23-30-39.mp4` and the matching
  runtime trace exposed one final intermittent case: after Metro changed the
  player root offset from about `1.80 m` to `1.26 m`, the original standing-only
  ownership test stopped tagging arrows. The stance interval above fixed that;
  the player confirmed the result.
- Release cleanup disables the temporary factory snapshot worker, binary arrow
  trace, preparatory field rewrite, and per-shot launch/release logging. The two
  functional hooks and one-time install/failure messages remain. Clean archive:
  `Builds/helsing-barrel-projectile-release-clean-20260903`; Release x64 built
  successfully with 0 errors. SHA-256:
  `252558542635D5860B2A5D5135C3664316A6311791A6A81C40856317FD6044F6`.
- Created `codex/scripted-events-safe-baseline` from `4763e6d`, the verified
  post-Helsing/pre-scripted-event build. This preserves the scripted-event
  behavior that worked before the regression while retaining the earlier VR
  fixes. The original `codex/pop-in-issue` branch remains preserved as the
  merged-scripted implementation for comparison and incremental porting.
- Audited the safe baseline's per-frame diagnostics. The memory scanner was
  already disabled, but the render-thread path still called the matrix-source,
  camera-matrix, command-stream, player-state, fire-coverage, and aim-write
  probe drivers. Gated those calls behind an opt-in runtime diagnostic flag,
  and disabled the fire-call debug-register fallback while retaining the
  functional native pneumatic constructor hook. This removes the known
  cross-thread/watchpoint frametime risk from scripted-event testing.
- Built and deployed the no-probe safe baseline successfully with 0 errors
  (one existing compiler warning). Installed SHA-256:
  `FAE5FBB9800C8BD28AF78EC6051AFB4B2AE933B51362959C9398B7089881BBC7`.
  Archive: `Builds/scripted-events-safe-baseline-no-probes-20260905`.
- User reported that this no-probe build made shots follow the head instead of
  the 6DOF gun. Root cause: `ArmFireProbe` also installed the functional inline
  gun-aim redirect before entering its diagnostic debug-register fallback. The
  cleanup disabled the whole function. Restored the inline hook while making a
  failed install terminate without entering the intrusive fallback.
- Rebuilt and deployed the corrected safe baseline with 0 errors. Installed
  SHA-256: `02E5C313695735A40AE0618A6C4E571FE25DA7147A24ACD130C02C6AE98D8F8D`.
  Archive: `Builds/scripted-events-safe-baseline-inline-fire-20260905`.
- Audit found that the diagnostic cleanup had also changed the fire-path
  failure behavior, despite restoring the inline gun-aim redirect. Removed
  that extra behavior and restored `ArmFireProbe` exactly to the known-good
  `4763e6d` implementation; only the clearly diagnostic per-frame calls remain
  gated. Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `1EC86D9B669E5C42E9C65FF5DCC0F52E1480A9900293623CB50F4F959E2C74D7`.
  Archive: `Builds/scripted-events-safe-baseline-diagnostics-only-20260905`.
- Ported only the scripted-event menu-confirm input fix. Chapter Select and
  Load Last Save can dismiss the menu without a later Start/B edge, leaving
  `sPauseMenuExpected` set and suppressing initial gameplay input until an
  unrelated action such as crouch. The confirm edge now clears that latch;
  no scripted camera, culling, or viewmodel ownership code was added.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `A4F18EA771DC6A90CE6B84F29AF1AA1721A54FD86950F49EED76CB56D8A159F2`.
  Archive: `Builds/scripted-input-latch-reset-only-20260905`.
- Ported the next isolated scripted-event change: reset the local scripted
  camera classifier and input-yield bookkeeping once per chapter/save loading
  transition. This prevents stale state from the prior scene without adding
  native camera ownership, culling, or viewmodel routing.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `160D46BEAA26472105F6E9EA3554AF55A155D23ED5D31950C011A9026C9E0FEF`.
  Archive: `Builds/scripted-load-state-reset-20260905`.
- User clarified that scripted events still render incorrectly on the safe
  baseline: the authored view is not forced correctly and hands separate from
  the arms. Added the first isolated visual-handoff pass. During the existing
  scripted-camera classifier window only, the renderer now bypasses the normal
  6DOF substitution for the native instanced viewmodel draw and submits the
  native draw/twin unchanged, preserving Metro's animated hand/arm connection.
  No native player-state ownership detection or culling changes were added.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `E0B4E46DC33AE87C3CAFF3EDE9AD4A04AD97B30143986485386CE8D1DC9233A0`.
  Archive: `Builds/scripted-native-hands-classifier-only-20260905`.
- The first visual-handoff test did not reproduce disappearing geometry, but
  the user observed the gun alternating between 6DOF and native head-space and
  the hands alternating between detached and arm-connected. This identified a
  handoff-state bug: the raw scripted-camera classifier was being consumed
  directly by the renderer. Added the original three-frame quiet release latch
  so a transient classifier clear cannot switch render routes mid-transition.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `63D0D6ECBF8E35A5546AC182A63117724DA4945C49504CE6C4138B5C98551A54`.
  Archive: `Builds/scripted-native-hands-stable-handoff-20260905`.
- User clarified from the attached video that the gun/hand route flicker occurs
  during ordinary gameplay as well as scripted scenes, with no reliable trigger.
  The experimental native-viewmodel bypass is therefore not a valid handoff
  fix: it exposes a conflict between the loose scripted-camera classifier and
  the 6DOF renderer. Removed that bypass from the active test source/build;
  native scripted ownership must be identified before routing viewmodel draws.
- User confirmed the rollback removed the flicker and that the retained input
  and load-state reset changes do not reproduce disappearing objects or the
  door-related 6DOF failure. Added Metro's native camera-effector ownership hook
  as an observer only. It reports transitions for the original exact authored
  camera/viewmodel flag families but has no rendering, culling, weapon, input,
  or viewmodel consumer, allowing the signal itself to be validated safely.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `3F39B4BCFE3E0AF6722AB6B88F0E46AC82BD26E96CA44004CEB8E00F7AADBCBC`.
  Archive: `Builds/native-scripted-ownership-observer-only-20260905`.
- The observer run covered both the door level that previously broke 6DOF and
  the disappearing-object test level after its scripted events. Metro's exact
  native viewmodel ownership signal produced three clean windows of roughly
  29-32 frames, each coinciding with a large authored camera turn. The exact
  camera flag never asserted. In contrast, the old pitch-divergence scripted
  camera classifier toggled repeatedly throughout the run, confirming that it
  is unsuitable for renderer routing and must remain excluded.
- Added the first consumer of the validated exact signal. While Metro reports
  native viewmodel-effector ownership, the matching hand/arm viewmodel draw is
  submitted in its native form (including the stereo twin); after a three-frame
  release debounce, normal 6DOF routing resumes. This change does not consume
  the loose classifier and does not alter camera ownership, world culling,
  input, door state, or firearm/projectile aiming.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `43359CCFC4F21F5D56F169CE068EDE3AF9E41A53032FE2BE1BAB075013478072`.
  Archive: `Builds/native-scripted-viewmodel-exact-signal-20260905`.
- User testing found no recurrence yet of disappearing objects or the
  door-related 6DOF failure, but hands remained disconnected during the tested
  scripted events. The exact native effector signal did assert twice for only
  34-36 frames, while the affected 21,252-index prologue/opening arm family was
  also present. This proves the short effector allow-list is not a complete
  lifetime signal and that hand families must not be treated as interchangeable.
- Returned to component-level restoration to avoid spending more headset runs
  on premature signal subdivision. Restored the complete scripted viewmodel
  component from the original implementation: native preservation for the
  21,252-index opening/prologue combined arm mesh, the town and post-clothing
  hand families, and other matching viewmodel draws. Its lifetime combines the
  exact effector observer with the previously validated player-performance
  fields (`player+0xF78/+0xF90`, `+0x15D0/+0x15D2`, and `+0x1850`). The loose
  pitch-divergence classifier remains excluded, and this state has no camera,
  culling, input, door, or projectile consumer.
- Rebuilt and deployed successfully with 0 errors (one existing compiler
  warning). Installed SHA-256:
  `76C8304B42BB851FBA88AC437D4DC84D9DCE89BE6B597F3A285C97DF8797764E`.
  Archive: `Builds/scripted-viewmodel-complete-component-20260905`.
- User testing isolated a regression in the complete viewmodel component: the
  problem door again broke weapon 6DOF, while disappearing objects and the
  other original failures did not recur. The immediately preceding exact-
  effector-only build did not break door 6DOF, so the new failure came from
  allowing the broader player-performance state to route every generic
  viewmodel draw into Metro's native head-space path.
- Narrowed player-performance ownership to the explicitly identified combined
  hand/arm families (opening/prologue, town, and post-clothing/Armory). It can
  continue to preserve their different scripted animations, but can no longer
  take ownership of the gun merely because it is an `IsViewmodelPass` draw.
  Exact native effector ownership retains the complete-viewmodel route. The
  loose pitch classifier remains excluded, and camera/culling are unchanged.
- Rebuilt and deployed successfully with 0 errors (one existing compiler
  warning). Installed SHA-256:
  `25A0EB9B429B0589B768145C9E691A09BAC3A7CD31586522AADE9456789DF059`.
  Archive: `Builds/scripted-hands-without-broad-gun-takeover-20260905`.
- The narrowed build partially fixed the door regression: the gun remained
  6DOF, but the visible arms and hands stayed connected to each other in
  Metro's native head/body-space pose instead of returning to the controllers.
  The supplied video `2026-09-05 14-47-55.mp4` shows this resulting state; it
  does not include the moment the door triggered it. The run contained no exact
  native-effector ownership transition, proving the stale player-performance
  route alone retained the arms.
- Existing saved captures already documented the cause: `player+0x1850`
  (`normalControl`) can remain zero after normal control has returned. Removed
  the old broad `(playerMode != 0 && normalControl == 0)` ownership clause and
  retained only the independently validated positive signals: the explicit
  owner/sentinel pair and layered mode `(1,1)`. Separate scripted hand-family
  routing remains intact.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `0DC795998ECD5EA9F79222F18B97CD0822494488C5BA64A87653A301D1552460`.
  Archive: `Builds/scripted-hands-ignore-stale-normal-control-20260905`.
- User confirmed this correction fully restored controller-based gun/arm/hand
  6DOF after the problem door while keeping hands attached to their arms during
  scripted events. The complete scripted hand/viewmodel component is therefore
  retained as a validated checkpoint.
- Added the next component as one isolated test: authored camera pitch control.
  Only exact native camera-effector ownership and the corrected positive
  player-performance states can now force the scripted view direction. The
  noisy motion classifier is no longer authoritative. Native release uses a
  fixed 12-frame smooth handback; no portal, visibility, object-culling, or
  broad camera-manager effect suppression from the original problematic merge
  was added.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `4B0009F11792D0891CFFEFF1BAC6C7269659F8D747737231885D1B12192765C8`.
  Archive: `Builds/scripted-camera-native-ownership-only-20260905`.
- User confirmed the door remained fully 6DOF, scripted hands remained
  connected, authored scripted view direction worked, and no disappearing
  geometry had appeared. The incomplete camera component did introduce a
  synchronized periodic flicker, gun bounce, and world shake.
- The run log showed native ownership itself holding for long intervals rather
  than rapidly flapping, but synthetic pitch steering repeatedly entered its
  suppress/resume/yield cycle around Metro's temporary camera effects. Restored
  the original stabilization pieces: every non-base native camera effector now
  pauses synthetic pitch steering for its exact lifetime and resumes through
  bounded recovery, and positive scripted player ownership preserves the whole
  native viewmodel so gun, hands, arms, and authored view cannot use conflicting
  routes. The stale `normalControl` ownership clause remains removed, preserving
  the validated door fix.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `CA19DFFC06815F41E0D94B6C006DC6F8628881FAC95984B4E4A67676AA745225`.
  Archive: `Builds/scripted-camera-stabilized-handoff-20260905`.
- User testing found that this build restored the disappearing-object failure.
  Door 6DOF, scripted hand attachment, and authored view direction remained
  correct, while periodic gun bounce/flicker/world shake was improved but not
  eliminated. The preceding camera-ownership-only build had not shown missing
  geometry, isolating the regression to one of the two stabilization additions.
- Removed only the broad native camera-effect pitch-steering suppression for
  the next decisive test. Earlier project tests already proved that suppressing
  engine-camera follow separates Metro's CPU visibility frustum from the VR
  view and causes geometry/pickups to disappear. Complete native viewmodel
  routing during positively identified scripted ownership remains enabled, as
  do the corrected player-state test, authored camera control, and hand fixes.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `D528C9EBC4273A7A1FFC77961F31342AC45D004753F377B2D11DFAB9E9CA2393`.
  Archive: `Builds/scripted-camera-without-effect-pitch-block-20260905`.
- User confirmed that disappearing objects no longer occur with broad native-
  effect pitch suppression removed. This validates engine-camera follow through
  ordinary additive camera effects as a required invariant and makes commit
  `509cb3b` the new safe checkpoint for the visibility regression. The remaining
  periodic gun bounce/flicker/world shake must be fixed without suppressing that
  follow. The run log shows long, stable native scripted-ownership intervals,
  so the remaining symptom is not explained by repeated ownership release and
  reacquisition.
- User clarified that gun bounce and flicker occur during ordinary gameplay
  regardless of whether a scripted scene has run, and do not appear to occur
  during the scripted scenes themselves. World shake is less frequent and its
  relationship to a preceding scene is not yet certain. This identifies the
  remaining false route as camera ownership during ordinary play rather than an
  unstable in-scene handoff.
- Separated hand-performance ownership from camera ownership. The validated
  player `(1,1)` state continues to preserve the authored hand/viewmodel
  animation, but can acquire camera control only after two frames of camera
  movement unexplained by our command while the HMD is still. Once acquired,
  camera ownership remains latched for that whole player-performance interval;
  the motion evidence is an acquisition gate only and cannot flap the route.
  Exact native camera descriptors remain independently authoritative. Broad
  native-effect pitch suppression remains absent.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `BCC17E4A3E0D1489BC91E0A057317F80C9442BE998CC300E99B1270B72A786F6`.
  Archive: `Builds/scripted-camera-gated-player-performance-20260905`.
- This acquisition-gate test failed: ordinary gun bounce, flicker, and world
  shake were unchanged, while scripted camera direction stopped working. The
  run contained no scripted-camera acquisition at all, proving those ordinary
  symptoms do not require the scripted ownership flag to be active.
- The decisive runtime pattern was instead the legacy pitch-follow controllers:
  throughout ordinary gameplay they repeatedly suppressed after 21 dead frames,
  entered yield after 45 frames, sent periodic probes, and resumed with camera
  errors sometimes exceeding 100 degrees. Disabled both periodic yield/probe
  and anti-windup suppression paths so engine-camera follow remains continuous.
  Restored direct positive player-performance camera ownership for correct
  scripted framing; confirmed scripted ownership still suppresses pitch follow
  directly, so the retired inference controllers are unnecessary there. Broad
  native-effect suppression remains absent.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `7B8086D192C05F0217F34CBF41B515540FB5E3CB534E80F9566D74C8D2D7B474`.
  Archive: `Builds/continuous-camera-follow-no-periodic-probes-20260905`.
- User confirmed the ordinary gun bounce, flicker, and world shake were
  unchanged, while scripted camera direction, scripted hands, door 6DOF, and
  object visibility all remained correct. The run contained no native
  viewmodel-effector transitions and no legacy suppress/yield events, ruling
  out both route switching and the retired periodic controllers for this
  remaining symptom. Outside scripted ownership, sampled engine pitch continued
  alternating several degrees above and below HMD pitch rather than converging.
- Added a behavior-neutral aggregate pitch-response measurement. It records the
  real engine-camera movement divided by the previous predicted command,
  average absolute error, and error sign reversals, emitting only one log line
  per 120 qualifying commands. This will determine whether the hard-coded
  `kPitchResponse=0.54`/full-gain correction is overshooting without risking a
  blind gain change that could restore culling and interaction lag.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `59385D280C50EBB0BAC2299BD462F3359C4B11E122DF16DA5AA44C3BC658A6F6`.
  Archive: `Builds/pitch-response-aggregate-diagnostic-20260905`.
- The aggregate response run ruled out a simple bad vertical-sensitivity
  constant. Later windows measured mean absolute response ratios near 0.96,
  while signed ratios remained only 0.15-0.44 and the pitch error crossed zero
  29-45 times per 120 samples. The engine is therefore applying approximately
  the predicted magnitude, but not on the immediately preceding rendered-frame
  boundary used by the controller. Full corrections can stack against an old
  camera reading and then reverse after the queued input lands.
- Replaced the one-frame aggregate with a behavior-neutral five-lag regression.
  It records frame-indexed pitch commands and reports correlation and scale for
  delays of one through five rendered frames. The strongest positive
  correlation will identify how many commands can be in flight, allowing the
  fix to compensate pending input instead of blindly lowering gain and
  restoring the already-proven culling/interaction lag.
- Rebuilt and deployed successfully with 0 errors. Installed SHA-256:
  `FA299DE406D7F25815427152302B386045E271AB3B3D833FCA57DF433C7C2B63`.
  Archive: `Builds/pitch-lag-regression-diagnostic-20260905`.
- User clarified that the remaining bounce/flicker occurs autonomously and does
  not require head movement. The lag regression then produced a decisive stable
  result: command lag 2 correlated 0.996-0.997 with observed camera movement,
  versus about 0.49 at lag 1, and scale was 0.978 relative to the existing
  prediction. This proves Metro applies the correctly sized command two
  rendered frames later; the full-gain controller was stacking a duplicate
  correction while the preceding command was still pending, then reversing
  after both landed.
- Removed the completed lag regression and changed only ordinary pitch follow
  to project the measured engine camera through the one preceding command still
  in flight. The controller now sends only the residual error. This preserves
  full-gain responsiveness and continuous camera/visibility follow while
  eliminating the stationary delayed-feedback oscillation. Scripted ownership,
  viewmodel routing, yaw, shooting, and visibility hooks are unchanged.
- User reported that the in-flight compensation appears to fix gun bounce,
  flicker, and world shake, but perceived lower framerate and some hitching.
  The run confirms the pitch loop is stable after scripted release (generally
  within about half a degree). GPU time instead scales with scene load: about
  14.3 ms in lighter samples and 19.1 ms at 1,612 doubled draws / 5,778 maps per
  frame. The compensation itself is one lookup and subtraction per frame and
  cannot explain that cost.
- Found older development telemetry still active independently of the camera
  work: an automatic four-frame full render-graph census, continuous GPU
  timestamp queries/GetData polling at Present, per-object high-resolution
  CPU timing around second-eye constant-buffer construction, and periodic
  performance reports. Disabled only those instrumentation paths. Actual
  stereo rendering and constant-buffer construction are unchanged, as are all
  camera, input, scripted-event, viewmodel, watch, and shooting paths.
- Rebuilt and deployed successfully with 0 errors (one existing C4250 warning).
  Installed SHA-256:
  `3C79E35CFC6646FEF0F2EA85EDCC84514C2DF6F93F768AC67C0487179A0C0D04`.
  Archive: `Builds/pitch-fix-with-performance-telemetry-disabled-20260905`.
- User reported that framerate and hitching remained worse than before the
  latest camera fixes. Comparing the current path against the smooth checkpoint
  found that each synthetic look command still performed a synchronous
  `GetCursorPos` / `SendInput` / `SetCursorPos` round-trip. A temporary A/B build
  disabled cursor pinning, but that candidate was withdrawn; cursor pinning is
  restored because it is required to keep synthetic camera input from moving
  the gameplay/menu cursor.
- The A/B with pitch compensation disabled restored a steady ~72 FPS in the
  same area, compared with roughly 65 dropping into the high 50s when enabled.
  This confirms the compensation path causes the performance regression, but
  not that extra geometry is definitively the mechanism. The user also
  confirmed compensation remains necessary for gun bounce/flicker/world shake.
- Restored cursor pinning and re-enabled pitch compensation. For the next
  optimization candidate, pitch and yaw correction are now delivered in one
  combined mouse INPUT whenever both axes need correction, preserving the same
  deltas and cursor behavior while avoiding a second input/cursor round-trip.
- User confirmed this combined-input candidate is substantially smoother. This
  validates that separate pitch/yaw synthetic-input delivery and its duplicate
  cursor-pinning overhead were the main performance regression, while pitch
  compensation and the cursor workaround remain enabled.
- Removed the remaining periodic culling-camera response logs (`yawresp`,
  `cullpitch`, and worst-error `cull` reporting). They were investigation-only
  output and are not part of the shipped camera-follow behavior. The functional
  delayed pitch compensation, cursor pinning, combined input delivery, scripted
  camera ownership, viewmodel routing, shooting, and visibility follow remain.
- The headset-validated release checkpoint is the exact combined-input build
  that held a steady 72 FPS in the test area while retaining pitch
  compensation and cursor pinning. Installed SHA-256:
  `4AD471D3387014AF2C6A150B8CF7AF5385C1AE1AA24D3688F4154FD054B64E57`.
  Source commit: `68cb4d1`. Archive:
  `Builds/pitch-compensation-combined-input-20260905`.
- Do not substitute the later rebuilt `611E27D0...` DLL for this checkpoint
  without a new headset performance validation. The exact `4AD471D3...` DLL
  was restored after the merge/cleanup regression and again ran without the
  reported hitching.

### Defense child-carry camera investigation parked — 2026-09-06

- The Defense-only HMD yaw under-response and smooth-turn coast begin when the
  child climbs onto the player's back and end immediately when he gets off.
  Native right-stick turning with the VR DLL disabled is normal.
- A continuous before/during/after trace proved that Metro stretches synthetic
  yaw input over roughly 15–20 frames during the carry. The fixed-delay render
  cancellation subtracts it too early, explaining both symptoms.
- A global measured-render-yaw/body-intent experiment fixed the original HMD
  and turning symptoms, but introduced imperfect walking direction and slight
  head coupling in the laser. Its load re-anchor fixed most of an initial
  wrong-facing regression, but the architecture was too broad for a problem
  isolated to one game state.
- All Defense behavioural changes are now removed from the active source path.
  The exact headset-validated 72 FPS DLL `4AD471D3...` is restored. Defense
  artifacts remain under `Builds/defense-*`.
- Full findings, rejected A/Bs, final-candidate behavior, performance cautions,
  prior XInput results, prompt-switching nuance, and the proposed disposable
  upstream-camera experiment are recorded in
  `Notes/33-defense-child-carry-camera.md`. Read that note before resuming.

### Defense post-scene watch timer candidate — 2026-09-06

- Separate issue: proceeding through the child-pickup scripted scene can move
  the custom HH:MM/filter timer off the physical watch and leave Metro's native
  timer on the HMD. Loading any checkpoint saved after the scene restores the
  correct wrist display, so this is a live-transition classification/parenting
  failure rather than bad persistent calibration.
- The existing iconless fallback accepted the first five generic six-index
  glyph draws using an atlas validated earlier from a real timer batch. The
  affected run log shows the unique timer icon disappears through the scripted
  interval while many unrelated generic-text matrices continue, allowing them
  to consume the five-glyph fallback window before the timer arrives.
- The physical route also treated every 6,108-index draw as its primary watch
  anchor even though the exact stable casing shader pair is known. A pooled
  count collision could therefore overwrite the custom display parent after a
  scripted resource transition.
- Candidate fix requires the exact 6,108-index casing shader pair
  (`79BA9F1ECFAF0CA1` / `3B3535E6C07D32BA`) for the primary Spartan watch
  anchor. That draw resets and authorizes the iconless glyph batch in the same
  frame; text encountered earlier cannot consume the timer slots. Icon-anchored
  chapters retain their existing path. No camera/input behavior or diagnostic
  readback was added.
- Release x64 built with 0 warnings and 0 errors. Candidate and installed DLL
  SHA-256:
  `116E342EA494BE8DE87A4A274F76B14699B33A1B7DBBE2F632C808AD4107EC59`.
  Archive: `Builds/defense-watch-timer-geometry-anchor-20260906`, containing the
  exact `4AD471D3...` smooth fallback as `d3d11.pre-change.dll`.
- Awaiting a continuous before-scene -> after-scene headset test. Also confirm
  ordinary watch time, active filter countdown, and unrelated HUD text remain
  correct. A post-scene checkpoint load alone is not sufficient because it
  already bypasses the reported failure.
- Headset result: **rejected immediately**. The watch stayed on its native right
  hand, the custom timer disappeared, and the native HMD timer still returned
  after the scene. The failed-run log proves Defense's actual 6,108 casing uses
  VS `79BA9F1ECFAF0CA1` but PS `6597099FD3B895DB`, not the older captured
  `3B3535E6C07D32BA`. The exact-pair predicate rejected the real casing; this
  disproves the assumption that the primary casing material is chapter-stable.
  The exact `4AD471D3...` baseline was restored before another change.
- Second candidate restores the proven physical-watch predicate byte-for-byte;
  no shader restriction remains. A successfully routed 6,108 casing merely
  resets and authorizes the same-frame iconless timer batch, preventing earlier
  unrelated atlas text from consuming its five slots without changing the
  watch parent. Release x64 built with 0 warnings and 0 errors. Candidate and
  installed SHA-256:
  `BE3C4EECBA0A6A4760B9A38D33B2B62BD9C4963819FB25D8C98D2AE7699968C6`.
  Archive: `Builds/defense-watch-timer-routed-casing-boundary-20260906`, with
  exact `4AD471D3...` fallback. Continuous scene validation is pending.
- Headset result: the physical watch correctly remained on the left hand both
  before and after the scene, but the custom timer still disappeared afterward,
  the native timer remained on the HMD, and the run hitched severely. Rejected;
  the exact `4AD471D3...` smooth DLL was restored immediately.
- The preserved log resolves the state boundary. The 6,108 casing continues to
  reparent successfully after scripted ownership releases, so physical watch
  parenting is not the timer failure. The scene lasts from roughly frame 2,730
  to 6,496, far beyond the custom display's 180-frame decoded-value lifetime.
  Afterward the atlas fallback activates repeatedly but never produces another
  valid four-digit decode. Therefore unrelated same-atlas glyphs still occur
  after the casing and consume the five slots; a casing boundary alone cannot
  identify the timer. The second candidate's source behavior has been removed.
- Do not attempt another suppression rule from draw order alone. The next test
  must capture the bounded set of same-atlas six-index candidates after the
  scene (matrix, texture, ordinal and asynchronously decoded glyph) and compare
  it with the timer batch after loading the same post-scene checkpoint. Keep the
  capture in memory or tightly bounded so it cannot recreate hitching.

### Defense timer bounded candidate diagnostic — 2026-09-06

- Added an evidence-only, one-shot census for the broken live-transition state.
  It arms only after the last valid watch decode has been stale for over 600
  frames while the physical left-hand watch route and validated timer atlas are
  both present.
- It copies no more than 32 six-index glyph quads from one frame, performs a
  delayed `DO_NOT_WAIT` staging-buffer map, writes one bounded group of log
  lines, then permanently disables itself for that process. It does not alter
  draw classification, suppression, watch parenting, camera state, or input.
- Each candidate records ordinal, decoded digit, UV origin, atlas identity and
  source UI matrix rows. This should expose which post-scene batch is the real
  timer instead of guessing from draw order again.
- Release x64 built with 0 warnings and 0 errors. Diagnostic archive:
  `Builds/defense-watch-timer-candidate-census-20260906`; the archive includes
  the exact smooth `4AD471D3...` baseline as `d3d11.pre-change.dll`.
  Diagnostic/installed SHA-256 is
  `D78F741871DDCA09DC2AB94BF47A6561A6E57658C85180C7133B6A0F741F23D1`.
- Test continuously from before the child-pickup scene until the timer is broken
  afterward, remain in gameplay briefly, exit, and preserve `d3d11_log.txt`.
  A separate working checkpoint-load census can be made afterward if the broken
  candidate set alone does not yield a stable classifier.
- Census result: capture frame 7,147 contained 13 eligible draws. Draws 0–7
  were non-decoding unrelated HUD text on `76d55570`; draws 8–12 were the real
  `10:49` clock plus punctuation on `35e3c17c`. The custom time visibly flashed
  on the wrist for a split second while the native HMD timer remained present,
  matching the transient valid decode in this sequence.
- Root cause: the successful icon-anchored learner committed textures for all
  five captured draws, even draws whose UV did not decode as a clock digit.
  This incorrectly validated both textures. After the long scene, the false
  `76d55570` resource arrived first and exhausted the global iconless fallback
  before the real clock resource.
- Fix candidate now learns a texture only from captured quads that individually
  decoded as one of the four clock digits. It does not hard-code either observed
  texture hash. The one-shot census and all `defensewatch` diagnostic code were
  removed. Release x64 built with 0 warnings and 0 errors.
- Candidate/installed SHA-256:
  `3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985`.
  Archive: `Builds/defense-watch-timer-decoded-texture-learning-20260906`, with
  the preserved census log and exact smooth `4AD471D3...` fallback.
- Continuous before-scene to after-scene headset validation remains required.
  Expected result: the custom display continues updating on the left wrist and
  Metro's native timer glyphs no longer remain on the HMD after the transition.
- **Validated in headset.** A continuous pre-pickup through post-scene run kept
  the custom time/timer on the left wrist and removed the native HMD timer as
  intended. The successful log learned only real digit texture `35e3c17c`
  (`set=1`); false atlas `76d55570` was absent even while iconless fallback
  remained active through the scene. The validated log is preserved as
  `Builds/defense-watch-timer-decoded-texture-learning-20260906/d3d11.validated-headset-run.log`.
  Fix `3EAB9B24...` remains installed; all census diagnostics are removed.

### Defense scripted-hand state diagnostic — 2026-09-06

- Remaining Defense issue: during the child-pickup performance, the hands do
  not appear connected to the authored arms. Normal gameplay hands are
  controller-attached. This occurs after the Armory clothing change.
- Existing code already includes native scripted routing for the ordinary
  12,132-index, post-clothing 21,762-index, and reused opening 21,252-index hand
  families. The last validated run logged the 21,252 family in controller mode
  near frame 715 without native ownership; the native effector asserted only
  much later at frames 9,878–9,904. The likely missing piece is Defense's player
  performance lifetime signal, not a missing mesh identity.
- Added a behavior-neutral, change-only diagnostic for the five known raw
  player state fields and the route chosen for each known hand family. Output is
  capped at 128 player-state, 32 opening-family, and 64 combined-family lines
  for the entire process. It performs no GPU readback and changes no routing.
- Release x64 built with 0 warnings and 0 errors. Diagnostic/installed SHA-256:
  `F2257455586238B804B3CA6D092A9AEA5F7ED57234F3E7E8202689112D28F5C9`.
  Archive: `Builds/defense-scripted-hands-state-diagnostic-20260906`, with the
  validated timer-fix DLL `3EAB9B24...` as the exact pre-change fallback.
- Test only from immediately before the child-pickup scene until normal control
  returns. The rest of the carry section and jump-off scene are unnecessary.
- Diagnostic result: Defense already asserts the supported layered performance
  state `(mode=1, performance=1)` from frame 2,793 through frame 7,184. The
  21,762 post-clothing mesh consequently enters native scripted routing. No new
  ownership signal or Defense-specific state gate is required.
- Root cause is hand-family priority. Metro submits the post-clothing hands and
  reused complete 21,252 arms/hands together. The gameplay authority rule
  discarded 21,252 before the scripted branch, leaving only the 21,762 hand
  submission even though the authored scene requires the complete arm mesh.
- Candidate gives the complete 21,252 family priority only during an already
  positive scripted-hand performance and suppresses the redundant 21,762 draw
  while that complete family is current. Outside that interval, ordinary
  post-clothing controller attachment and suppression rules are unchanged.
- Removed all `defensehands` state/route logging. Release x64 built with 0
  warnings and 0 errors. Candidate/installed SHA-256:
  `A1DE597F0D370DC69D14706EBD4BA72839EBAD59939B6790557CA4F427135D62`.
  Archive: `Builds/defense-scripted-complete-arms-priority-20260906`, containing
  the state evidence and clean `3EAB9B24...` timer-fix fallback.
- Validate only the pickup performance: hands should appear attached to the
  authored arms, then normal controller-attached post-clothing hands should
  resume when the scene releases.
- Headset result: **rejected**; the hands remained absent. The assumption that
  the 21,252 mesh observed earlier remained current during the `(1,1)` scene
  was unsupported. The priority behavior has been removed completely.
- Added a bounded, evidence-only scripted geometry census. While the validated
  hand-performance state is active it logs each unique non-instanced viewmodel
  draw and unique instanced viewmodel/matrix-layout draw once, including exact
  index range, instance count, shader pair, pass/layout classification and
  pending instance-buffer state. Both categories cap at 256 signatures; there
  is no GPU readback or routing change.
- Release x64 built with 0 warnings and 0 errors. Diagnostic/installed SHA-256:
  `01B54F9E450DC7DBBF954170320665C6E94BB9E60C4A8E37D82D8F1C98283925`.
  Archive: `Builds/defense-scripted-geometry-census-20260906`, with the clean
  validated timer-fix fallback `3EAB9B24...` and the failed-priority run log.
- Replay only the pickup performance, exit, and inspect `VRPose defensehands
  census` lines. This census must identify the actual authored arm draw before
  another visual routing candidate is attempted.
- Census result: the correct 21,762-index post-clothing hand mesh is submitted
  continuously and already takes the native scripted route. The prominent
  `10,875`, `5,292`, and `4,176` viewmodel signatures were initially suspected
  as authored arms, but offline post-VS renders identify them as the weapon
  body, ammunition loops, and canisters. The 21,252 complete prologue family
  is absent during the Defense scripted interval. Do not reroute those weapon
  components as a hand fix.
- Removed the broad census and added one-shot pose capture for the exact
  21,762 mesh: one 64-byte native instance plus b8 bone palette before the
  performance and one pair after positive scripted ownership begins. It makes
  no rendering change and permanently disarms each phase after a successful
  capture. Release x64 built with 0 warnings and 0 errors. Archive:
  `Builds/defense-scripted-hand-pose-capture-20260906`; diagnostic/installed
  SHA-256:
  `D5064B6EFF654886DEA4B5F86E90210D72D99350A9449CE2CEDD65A08A7C8E11`.
- Both pose captures succeeded. Offline skinning proves the 21,762 mesh has a
  coherent authored pose before and during the scene: both hands and cuff
  details move into a compact near-camera pose roughly 0.35–0.65 m below/in
  front of the origin. The palette has no collapsed, invalid, or distant
  components, and the native instance is also valid. Authored animation and
  placement are ruled out.
- The ordinary controller-hand route already disables Metro's viewmodel
  scissor rectangle because it can erase both hands at the lower headset edge;
  the native scripted route lacked that protection. Added the same raster-only
  handling solely for recognized 12,132/21,762 hand draws during positive
  scripted-hand ownership, including reapplication for eye 1 and exact raster
  restoration afterward. Gun, watch, world, ordinary gameplay, and generic
  native-effector draws are untouched. Removed all capture/readback code.
  Release x64 built with 0 warnings and 0 errors. Archive:
  `Builds/defense-scripted-hands-no-scissor-20260906`; candidate/installed
  SHA-256:
  `6E9F21FCEFE78B5B3BF8CCF769EF3C0901052F413E0E100994ED85C4FC5B4CE3`.
- Headset result: **rejected**; disabling the scissor for the native scripted
  hand draw did not make the hands appear. The user considers this a minor
  issue and asked to park it. Removed the no-scissor behavior and left no hand
  diagnostic/readback code active. Source and installed DLL are restored to
  the exact validated Defense timer-fix checkpoint, SHA-256:
  `3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985`.
  Preserve the hand investigation archives as evidence only; do not continue
  this issue unless explicitly requested later.
- Final release audit: tracked `HackerContext.cpp` and `VRPose.cpp` have no
  delta from validated timer checkpoint `db34711`; the installed and archived
  timer DLLs are byte-identical at `3EAB9B24...`. Custom watch rendering,
  native-HMD timer suppression, and decoded-glyph-only atlas validation are
  compiled into the DLL and enabled. They have no runtime dependency on a
  Defense patch, trace, capture, texture list, or other external file; dynamic
  atlas learning is deliberate resolution/quality-independent core behavior.
- Confirmed no enabled Defense diagnostic/census/pose-capture switch and no
  `defensehands` hook remains. Deleted the two leftover Defense camera CSVs and
  four hand-pose buffers from the game directory. Copies remain recoverable in
  the appropriate ignored `Builds/` evidence archives. The installed DLL
  remains the exact user-validated `3EAB9B24...` artifact.

### Direct internal-camera HMD follow — isolated investigation, 2026-09-06

- Began from `master` at `dc0248984c521d80864beb526b26272b379d9d6a` on
  `codex/direct-camera-hmd-look`. Read the complete status log and Notes 13,
  24, and 33 before changing source. Scope is only replacement of synthetic
  mouse camera follow; direct rendered HMD pose remains unchanged.
- Reconfirmed the installed and archived rollback DLLs are byte-identical at
  SHA-256 `3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985`.
  The installed DLL was not replaced. The older `4AD471D3...` artifact remains
  a headset-only 72 FPS comparison, not a performance claim for this build.
- Static command-stream decoding identified camera opcode `0x70000014` and its
  unique producer at `metro.exe+0xD6BE0`. Unlike the known-late `+0x7E09A0`
  replay setter, this function consumes a 4x4, derives recorder camera state,
  and then emits the camera command. The old `+0x8373FF` hit remains rejected
  as a reused batch-matrix slot.
- Added a disposable, signature-checked hook at `+0xD6BE0`, inert until Shift+F10.
  It applies a five-degree yaw to a private matrix copy only for the inferred
  dynamic main-view caller at `+0x817B4E`. Mouse follow/cancellation and the
  combined `SendLookAxes` delivery remain intact. No periodic trace, readback,
  or per-frame logging was added.
- Release x64 built with 0 warnings and 0 errors. Undeployed diagnostic SHA-256
  `C0EA1631D86F1251D6EBAB1A4C1E98233BE736E15B0A3A30106638F0BE7B66EC`;
  archive `Builds/direct-camera-construction-probe-20260906`, including PDB and
  exact `3EAB9B24...` rollback. See Notes/34 for evidence and the four-part
  headset acceptance test. Do not remove mouse follow unless the same probe
  moves rendered view, internal forward, CPU culling, and interaction together.
- Initial headset observation: `Shift+F10` produced a brief noticeable response,
  confirming that the toggle/hook executed, but did not cause ongoing hitching
  or a performance regression. No obvious rendered-view change was seen; the
  direct `PatchMappedVRCameraData` HMD-render path is expected to mask that
  rotation. Culling, interaction, and shooting alignment remain unproven, so
  the result is **execution confirmed, authority not yet proven**. Mouse follow
  remains enabled.
- Runtime log review showed no direct-camera install or activation lines for
  the first deployed build; its F10 response was the existing 3DMigoto config
  reload binding. Added a bounded one-shot signature-gate log, rebuilt Release
  x64 with 0 warnings/errors, and deployed v2. Installed/archived v2 SHA-256:
  `E1A99A554A6E539C1EE7E6E34DA6AAD7BCD5817109D1C367116034DA35F47236`.
  Restart Metro before the next `Shift+F10` test. The exact `3EAB9B24...`
  rollback remains preserved; mouse follow is still enabled.
- Per explicit user instruction, deployed only that diagnostic DLL to the game
  directory for controlled headset testing. Pre-deployment installed hash was
  verified against the preserved rollback `3EAB9B24...`; post-deployment hash is
  exactly `C0EA1631D86F1251D6EBAB1A4C1E98233BE736E15B0A3A30106638F0BE7B66EC`.
  No source behavior was changed for deployment, no mouse follow was removed,
  and no headset result has yet been claimed.
- v2 log review found the signature gate was one byte short: live
  `+0xD6BE0` begins `40 53 48 83 EC`, while v2 expected `53 48 83 EC ...`.
  Corrected the gate, rebuilt Release x64 with 0 warnings/errors, and deployed
  v3. Installed/archived v3 SHA-256:
  `487AF5926A8FE3E1C0926C1E690AD474230791F0E846B4AB1C817B1A5CA4A58A`.
  The exact `3EAB9B24...` rollback remains preserved. Restart Metro before
  testing Shift+F10; require install/ACTIVE/selected-callsite log evidence
  before interpreting the probe result.
- v3 installed and toggled, but no selected-callsite hit was logged. Added a
  bounded up-to-eight-entry active caller return-RVA report to identify the
  actual camera-construction caller, rebuilt Release x64 with 0 warnings/errors,
  and deployed v4. Installed/archived v4 SHA-256:
  `DC7025665666A253EEE146B51BDCCE6E07D642F3A1D9FB243C76047D2101FDB5`.
  This remains a disposable probe; mouse follow is unchanged and the exact
  `3EAB9B24...` rollback is preserved.
- v4 caller report identified `+0x846F33` as the dominant per-frame return;
  disassembly confirms its preceding instruction `+0x846F2E` calls
  `+0xD6BE0`. Selected this evidenced caller, rebuilt Release x64 with 0
  warnings/errors, and deployed v5. Installed/archived v5 SHA-256:
  `52E79C0C4E2657448AD55C1103A845140D11FD089ECF3D867193D58FE37BC916`.
- v5 rejected its own validation because `+0x846F33` is the return address
  rather than the CALL start. Corrected the validated CALL RVA to `+0x846F2E`
  (return `+0x846F33`), rebuilt Release x64 with 0 warnings/errors, and
  deployed v6. Installed/archived v6 SHA-256:
  `664986EE63045715E48E4D903DA78D09340190787336AD8633380CFF29848479`.
- v6 headset run confirmed repeated selected-callsite hits while ACTIVE. The
  user observed shadows disappearing with the probe enabled, providing initial
  evidence that this upstream construction affects camera-dependent engine
  behavior (likely visibility/shadow selection), not merely a late picture
  patch. HMD rendering masks the diagnostic rotation; interaction and shooting
  alignment are still unproven. Mouse follow remains enabled.
- Added a temporary VR-native toggle for the probe: press both controller
  grips together. It emits a one-line in-headset overlay and preserves all
  existing controller mappings otherwise. Release x64 built with 0
  warnings/errors and deployed v7, hash:
  `6ADE7C8FCCE6F93A6D47CC51F96B61EFC4837416831A064143EE4702298540F7`.
- v7 headset result: the both-grips chord and selected `+0x846F33` hook hits
  were confirmed. The overlay is not visible in the VR mod. Interaction,
  shooting, and general play appeared correct; shadows disappearing was the
  only clear probe regression. The existing post-Defense HMD/right-stick issue
  was unchanged, which is expected because mouse follow remains enabled and
  the fixed-offset probe does not alter Defense yaw stretching. This supports
  the upstream point as a replacement candidate but does not yet validate the
  real HMD-delta implementation.
- Built the first reversible real-HMD candidate at the validated upstream
  construction call. Enabling with both grips snapshots and removes the
  synthetic yaw/pitch already resident in Metro's camera, composes the current
  HMD rotation, preserves camera world position by recomputing view translation,
  and suppresses mouse/stick follow emission and booking. Disabling resumes the
  unchanged legacy follow on the next frame. Removed the ineffective overlay
  and bounded caller report. Release x64 built with 0 warnings/errors and
  deployed v8, SHA-256:
  `10CC8251E7DF39C5E705145C1481C0222B1F5604C320510AF3F09DEEF56C4C09`.
  Exact `3EAB9B24...` rollback remains archived with the candidate.
- v8 headset result rejected the selected point. With direct mode active,
  physical HMD turning did not move Metro's internal camera, controller-relative
  locomotion became wrong, and geometry failed to appear. Runtime
  rendered-vs-engine yaw divergence grew to roughly 50–100 degrees and returned
  to about one degree when legacy mode resumed. This proves `+0x846F2E` is a
  downstream shadow/render submission, not the authoritative player-camera
  construction point. Restored the installed DLL immediately and verified exact
  SHA-256 `3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985`.
- Disabled the rejected direct-follow hook and added a behavior-inert one-shot
  hardware write watch for the main source matrix at `metro.exe+0xD22F20`.
  It arms automatically 120 gameplay frames after startup, captures one writer
  with registers/stack, and permanently disarms; no hotkey or periodic trace is
  required. Release x64 built with 0 warnings/errors and deployed v9, SHA-256:
  `C95D26181DDBD3471F820366E04296AD2FA9D1D2C40DC137DE56111E3B1BC179`.
  Exact `3EAB9B24...` rollback remains archived with the diagnostic.
- v9 caught `metro.exe+0xD53A9`, a 704-byte state-copy routine called from
  `+0x7DF1E4`. Registers plus disassembly reveal the buffered matrix chain
  `+0xD07730` -> `+0xD23270` -> `+0xD22F20`. Moved the automatic one-shot watch
  to the incoming `+0xD07730` matrix, kept rejected direct follow disabled, and
  rebuilt/deployed v10 with 0 warnings/errors. SHA-256:
  `7197FEBA143BE9B76D7F00F84ACDE212C83BA7ED32BD445A19FFCFF6476A8712`.

- **2026-09-06 — direct-camera investigation: v10 resolved the active upstream
  camera construction chain.** The automatic watch at `+0xD07730` fired after
  the store at `+0xD626` in the rigid view constructor `+0xD560`. Its caller is
  `+0x69B900`, reached from `+0x1F38A0` through the unique call at
  `+0x1F3960`. The higher-level frame path `+0x2065F0` resolves the live camera
  object and calls virtual slot `+0x1220`; all three observed camera vtables
  map that slot to `+0x1F38A0`. That method updates the object-owned basis at
  `object+0x640`, while `+0x69B900` normalizes/orthogonalizes its inputs and
  publishes the related globals at `+0xD076F0/+0xD07730`. Prepared the next
  reversible probe at `+0x69B900`: selected caller only, private copies of both
  orientation vectors rotated together by 15 degrees, controller-grip toggle,
  and legacy mouse/cancellation left intact. Removed the completed v10
  one-shot writer watch from the play path. Release x64 built with 0 warnings
  and 0 errors; archived/deployed v11 SHA-256
  `2164241616247BD1413E677DCB96351AB8B77B43B96941ED460690FAD066108F`.
  Verified the installed copy matches and preserved the exact validated
  `3EAB9B24...` rollback alongside it.

- **2026-09-06 — direct-camera investigation: superseded fixed-angle v11 with
  full upstream HMD A/B candidate v12 before headset testing.** Active mode at
  the selected `+0x1F3960 -> +0x69B900` constructor now captures the HMD
  yaw/pitch reference, applies subsequent tracked yaw/pitch to private copies
  of Metro's incoming forward/up vectors, and suppresses synthetic mouse/stick
  camera follow. Position and object-owned source state remain untouched, and
  current non-delayed cancellation prevents the existing rendered HMD patch
  from doubling yaw/pitch. Roll remains in the proven rendered path only.
  Disabling restores the frozen legacy injection baseline/history so mouse
  follow resumes cleanly. Release x64 built with 0 warnings and 0 errors;
  archived/deployed v12 SHA-256
  `B640A441D0F97D3CEBE4584FD7C0230459A1B5C07CA8629036CCD94E02516B78`.
  Exact `3EAB9B24...` rollback preserved alongside it.

- **2026-09-06 — direct-camera v12 headset result: upstream authority proven;
  v13 corrects locomotion and scripted ownership.** In direct mode, physical
  HMD turning retained geometry and gunshots followed the intended aim;
  rendered-vs-engine alignment was normally about 0.1–0.3 degrees. Remaining
  failures were movement staying in the unchanged source/body frame and
  forced scripted views being overridden. v13 rotates Metro's native left
  stick by the same activation-relative HMD yaw. It yields `+0x69B900` to the
  original vectors while a native camera, scripted-viewmodel, or validated
  player-performance owner is active, keeps synthetic follow quiet during the
  yield, and re-anchors the HMD on ownership release. Release x64 built with 0
  warnings and 0 errors; archived/deployed v13 SHA-256
  `420209A4FA5C75DC335E8F3F095232526334F7DA7D030C4B2F75966714235097`.
  Exact `3EAB9B24...` rollback preserved alongside it.

- **2026-09-06 — direct-camera v13 headset/video result: locomotion fixed, but
  scripted/menu/merchant yield was incomplete; v14 hands off the whole legacy
  controller.** Physical-turn locomotion now follows the intended heading, and
  geometry plus shots still track physical HMD yaw. Forced views remained
  broken at menus, merchants, and scripted scenes. The supplied video also
  captured presentation/culling-type issues and a gun-swap prompt that only
  recovered after toggling to legacy mouse follow and back. Runtime evidence
  showed why: v13 passed Metro's original basis during an authored yield while
  direct cancellation and mouse suppression remained active, allowing about
  100-164 degrees of rendered-versus-engine divergence. v14 now uses the
  complete proven legacy mouse/cancellation path during non-gameplay, menus,
  authored-camera/viewmodel, and player-performance ownership. It debounces
  release for eight constructor frames, captures the refreshed legacy state,
  and re-anchors the HMD before resuming direct mode. Direct activation also
  begins with the same eight-frame legacy settling pass, automating the manual
  refresh that restored the interaction prompt. Normal gameplay remains on the
  direct mouse-free path. No periodic play-build diagnostics were added; the
  both-grips A/B toggle and exact `3EAB9B24...` rollback remain available.
  Release x64 built with 0 warnings and 0 errors; archived/deployed v14
  SHA-256 `549375BB8139192155B2FADA990ECE0DA06E2CEA07920E72AA3C1DF14D73B8E9`.
  The exact fallback is verified beside it in
  `Builds/upstream-direct-camera-full-legacy-handoff-v14-20260906`.

- **2026-09-06 — v14 partial headset success; v15 corrects the direct basis.**
  The tested scripted scene works, but vendor forced views, disappearing decals,
  and gun pickup remain faulty. Physical right turn plus left thumbstick turn
  recovered pickup. The archived video69-1 runtime log shows roughly 20-25
  degrees of engine/render disagreement after scripted release, often with
  small yaw error. Direct camera-local matrix composition retained source pitch
  while rendering uses world-level yaw and absolute HMD pitch. v15 now builds
  the upstream forward/up from source yaw plus relative physical yaw, and HMD
  pitch plus the same scripted release easing and 85-degree cap as rendering.
  Release x64 compiled successfully; numerical basis checks passed. No new
  diagnostics or speculative vendor classifier were added. Vendor ownership
  remains unresolved and the candidate needs headset validation for visibility,
  pickup, and scene release. SHA-256:
  `360DC9CE21272808FBBEE9EBEF1123C99FD160FEC1B936828C2998FE1E5F46CC`.
  Both v14 and exact `3EAB9B24...` rollback DLLs are preserved in
  `Builds/upstream-direct-camera-level-basis-v15-20260906`.

- **2026-09-06 — v15 improves pickup/culling; v16 targets physical-turn sprint.**
  The headset run confirmed improvement but exposed upper-view visibility loss
  and failed sprint away from the original physical heading. Log alignment is
  frequently 0.0-0.3 degrees, with brief roughly 5-degree errors. Disassembly
  confirms native sprint predicate `+0x28E320` requires the forward movement
  bit, which pre-rotating the left stick can remove. v16 preserves native stick
  input and instead rotates the completed world movement vector from
  `+0x28D110`, restricted to its local-player caller `+0x1F32C5`. Native sprint,
  stamina/weapon restrictions, and vertical motion remain native. Failed hook
  validation retains v15 stick correction. Release x64 compiled successfully;
  SHA-256 `511F23A76E8E952BCB1AAF0189EE71DD3BEC89F96C0B94CC3E565765A5023972`.
  Exact baseline and v15 rollback DLLs are preserved with v16. Headset testing
  remains required. Upper-view visibility and vendor forced heading remain open;
  no culling change or new periodic diagnostic was introduced.

- **2026-09-06 — v16 rejected; v17 native look/body-state prototype built only.**
  User rejects movement/sprint workarounds. Exact `3EAB9B24...` baseline is
  restored and remains installed. Baseline external reads confirm mouse input
  changes native look pitch/yaw and body yaw. v17 hooks `+0x28BFC0/+0x8FE740`
  before Metro's own look/body setters, preserving real-input processing and
  native angular limits while crediting only the separately applied HMD delta.
  No private camera-basis or movement-output hook is installed; raw stick
  rotation is disabled. Both-grips A/B remains, starts in legacy, and preserves
  actual native rotation credit on disable. Direct selection suppresses
  synthetic HMD follow. Live menu input-owner methods were identified and
  statically checked; vendor/headset behavior is still unverified.
  Release x64 succeeded; six offline math/signature tests passed, not a claim
  of native runtime or headset validation. SHA-256:
  `4E6CB5D6A2A69DDDBDA17EAEEBABBCEE0CDFF7C2562464F6B8A8D7E041FE6621`.
  `Builds/native-state-follow-v17-20260906` preserves DLL/PDB/source and exact
  baseline rollback. Not deployed; game was running the baseline for read-only
  inspection. Temporary one-shot native-setter confirmation must be removed
  after testing; no new periodic diagnostics. See Note 34 for limits, rejected
  preflight, transition-settling caveat, and required full revalidation.

- **2026-09-06 — v17 deployed for deliberate headset A/B testing.**
  User confirmed game exit; process absence and candidate/baseline hashes were
  verified before deployment. Installed DLL now verifies as
  `4E6CB5D6A2A69DDDBDA17EAEEBABBCEE0CDFF7C2562464F6B8A8D7E041FE6621`.
  Exact `3EAB9B24...` rollback remains verified in the v17 archive. No settings
  changes. Starts in legacy mouse follow; both grips toggle native-state follow.
  Headset validation pending; no merge or performance-equivalence claim.

- **2026-09-06 — v17 world-stability regression; v18 bounded diagnostic.**
  User's 21:15:02 video shows cabinet/barrel geometry moving relative to walls.
  Native hook activation and look/body setter updates are confirmed, but do
  not establish per-draw camera coherence. Evidence archived under
  `Builds/native-state-v17-video-211502-review`. v18 preserves behavior and
  adds a one-shot ten-second CPU-memory matrix capture with one background
  binary save, no GPU readback or periodic file logging. Not a fix/play candidate.
  Release x64 built and tests passed. Deployed with Metro absent; installed
  hash verified `D0F953B7F6A6C642868E69428D50B8755165E249B30FA88C1C249AEFA7040ABC`.
  Archive `Builds/native-camera-coherence-v18-20260906` preserves source/PDB/DLL
  and exact verified baseline rollback. Remove capture instrumentation after
  diagnosis. No settings changes or merge. Awaiting one ordinary-gameplay
  reproduction using both grips; assistant will inspect the capture.

- **2026-09-06 — v18 captured; v19 adds the missing draw-use stage.**
  v18 saved 7,414 records. All 659 significant sampled object/view mismatches
  were the first object of a frame; its inferred view matches the camera
  submitted later in that same frame. Cached view matches the preceding frame
  in all 673 available comparisons. Subsequent sampled objects agree; submitted
  camera yaw is close to native look and the two sampled camera writes share
  the same cancellation. This establishes a map-order mismatch, not yet that
  the stale object version reaches a visible draw. v19 records suspect-buffer
  lifetimes, left-eye draw preparation, bound shaders and viewport without
  changing rendering. Same ten-second fixed-memory/background-save diagnostic.
  Release x64 and offline checks passed. Installed with Metro absent; SHA-256
  verified `F8D867564D4F65927B77EE4ABB1B1F9BB270FAE8F8BD0D3695B10DCFFAEA154D`.
  Archive `Builds/native-camera-draw-use-v19-20260906` preserves exact rollback.
  One repeat capture needed; no fix or performance claim, settings unchanged,
  branch unmerged. Remove temporary instrumentation after diagnosis.

- **2026-09-06 — v19 confirms early depth camera mismatch; v20 deployed.**
  Retrieved the user's broader reproduction: all 613 significant first-object
  mismatches reached left-eye depth draw preparation using VS 1FB92D15F7533B51.
  Their reconstructed camera matches the later same-frame camera. v20 refreshes
  shared eye-camera caches from validated early object matrices before applying
  the existing object patch. No per-prop, movement or forced-sprint workaround.
  Temporary v17/v18/v19 diagnostics removed and archived. Release x64 succeeded;
  six native-follow and three reconstruction tests passed, including all 613
  captured mismatches. Not headset/performance validation or proof all symptoms
  share this cause. Deployed with Metro absent; installed SHA-256 verified:
  `55C2AFE30AF1DD06C067D8131FE5741CB9C947070BF8191D6D061533DEF590B9`.
  `Builds/native-early-camera-sync-v20-20260906` preserves DLL/PDB/source and
  exact rehashed 3EAB9B24 baseline. Starts legacy, both grips toggle native.
  No settings change or merge. Next: same-area headset A/B stability check,
  followed by the full validation list if improved. See Note 34.

- **2026-09-06 — v20 fails moving-object headset check.**
  User still sees moving objects. Assistant verified installed v20 hash and
  native/legacy/native toggle events, and archived the runtime log with v20.
  Early-depth reconstruction alone is not a complete diagnosis; this log cannot
  verify runtime guard acceptance or camera-baked instance coherence. Next
  investigation must establish that evidence before another fix/test request.
  No new deployment or settings change. Exact rollback preserved; branch
  unmerged and v20 explicitly not accepted.

- **2026-09-06 — v21 bounded CPU instance/camera diagnostic deployed.**
  Observes existing vertex-buffer writes and matrix-instanced draw entry,
  pairing raw instance data with raw bound/prepared eye camera matrices and
  shader identities. Also records whether v20's early cache refresh executes.
  No renderer/native/input changes or extra GPU maps/readbacks. Ten seconds,
  bounded sampling/memory, one background binary save; not a fix/play build.
  Release x64 built and 12 offline tests passed. Installed with Metro absent,
  hash verified `1A10FD6E1E8C9E49EFAE7EF2363E1C717BED311AC15244A25DE197F35B9A4F87`.
  Archive `Builds/native-instance-coherence-v21-20260906` preserves exact fallback.
  Test in affected area while stationary, both grips then head turns for 10–15
  seconds. Assistant retrieves evidence. Remove instrumentation after analysis;
  no settings change, merge, headset success or performance claim. See Note 34.

- **2026-09-06 — v21 analyzed; exact accepted DLL restored.**
  Capture saved 11,740 records; v20 refresh succeeded on 668 frames. All 8,304
  prepared draw camera/correction pairs agree to roughly 0.0000024; folded
  projections also agree. 462 same-frame instance samples include 129 rigid
  props, largely recurring stable world transforms. Skinned bone data and
  actual post-override GPU consumption remain unproven. No evidence-backed
  reason for another shared matrix fix from this capture. Return to native
  mouse/hook sequence comparison; F50 activity-time gating is a concrete
  parity question, not a diagnosed cause. Evidence archived with v21 and
  runtime instrumentation removed. With Metro absent, restored and verified
  exact `3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985`.
  No settings changes, merge, or further headset test requested. See Note 34.

- **2026-09-06 — narrowed native activity-gate hypothesis; external check ready.**
  F50 timeout only matters with a configured live native camera owner; it is
  not a general HMD activity flag. Normal eligible look processing still calls
  native angular/look/body setters with zero pending mouse input. No evidence
  for another renderer fix or unconditional activity stamp. Extended read-only
  external observer to report the exact gate conditions and source vectors;
  five branch-model tests pass. No C++/DLL/settings changes. Need accepted
  baseline running in the affected area for a no-input read-only check;
  headset can be removed after loading. See Note 34 for static evidence/limits.

- **2026-09-06 — live baseline rules out activity timeout in observed area.**
  Read-only checks show F92=0, no owner, zero timeout, and unblocked native look
  processing despite an old F50 timestamp. No activity-stamp patch justified.
  Look/body/source orientation agrees; shared published-forward global can
  transiently show +Z on the accepted baseline, so pass-unaware external reads
  must not be mistaken for a regression. Retained sample subset archived under
  Builds/native-owner-readonly-20260906. Exact accepted DLL unchanged, no new
  test build, input generation, settings changes or merge. See Note 34.

- **2026-09-06 — existing v21 capture proves cross-pass cancellation mismatch.**
  Revised the earlier overly broad negative matrix conclusion: 3517 sampled
  draws across 102 frames use different corrected views for identical native
  camera matrices. Maximum forward difference 2.5741 degrees; changed-view
  yaw discrepancy matches the cancellation credit advance within 0.000016
  degrees. Per-draw internal consistency missed this shared timing defect.
  Added chronological cross-pass analysis and two regressions (six tests pass).
  Next: associate cancellation with native camera production/consumption,
  not guessed lag or per-prop adjustment. No candidate or new headset request;
  exact accepted installed DLL reverified. See Note 34 for evidence/limits.

- **2026-09-06 — traced camera-credit publication/copy/command lifetime.**
  Confirmed normal and effector matrix construction, old/new camera-copy order,
  an optional buffered-camera rewrite, and the record-to-stack replay boundary.
  Reading the current credit or matching only a matrix/pointer remains unsafe.
  Added an offline immutable-generation sidecar model; ten tests pass plus six
  capture-analysis tests. Runtime command identity and synchronization still
  need integration; this is not a deployed fix or headset-ready build. No C++
  hooks, installed DLL, settings, Defense fix, or merge changed. See Note 34.

- **2026-09-06 — implemented fixed-capacity C++ command-credit storage.**
  Confirmed recorder capacity handling can synchronously replay or queue/pool
  a buffer before emitting the new camera packet. Implemented allocation-free
  recording and independent replay snapshots with epoch/serial validation,
  explicit unknown entries, and failure on ambiguous reuse or ordering.
  MSVC optimized x64 /W4 /WX build passes 98 assertions; 16 Python tests pass.
  Component is not yet wired into runtime hooks or the view patch, so this is
  not a fixed/deployed game build. Accepted installed DLL remains untouched.

- **2026-09-06 — v22 runtime camera-credit integration built and deployed for A/B validation.**
  Metadata now follows native publication, buffered camera copies, recorded
  commands and ordered replay; matched credits feed view correction and shake
  measurement together. Native look/body/input behavior is otherwise unchanged.
  Five signatures verified; 48 actual-adapter mock assertions, 98 storage
  assertions and 16 Python tests pass. Release x64 build successful.
  Experimental limitation: unmatched packets retain the old cancellation path;
  event-only match/miss/fault summaries must be checked before claiming coverage.
  Installed hash 74EE1B910E8AF0681CA3FEB8BD234BBDD054E4286E4A3DEB7EABE52718B66883.
  Exact 3EAB9B24... fallback and guarded restoration script archived beside it.
  Starts legacy; both-grips native/legacy A/B. No merge or performance claim.

- **2026-09-06 — v22 positive headset result: moving objects and merchant orientation.**
  User reports objects no longer move with HMD and merchant screen now forces
  the correct direction. Log confirms two active native-mode intervals with
  1,125,154 matched camera-credit queries, 396 unmatched, zero adapter faults
  (99.9648% eligible-query coverage). Misses are not yet classified; do not
  infer harmless transitions or full parity. Log archived beside v22; candidate
  and exact rollback hashes reverified. No deployment/source change or merge.
  Final cleanup must also audit older periodic diagnostic output in this log.

- **2026-09-06 — v23 bounded miss breakdown deployed; successful correction unchanged.**
  v22's totals cannot retrospectively classify the 396 misses. Added bounded
  in-memory reason/frame/gap examples and ownership-transition timing, emitted
  only at mode/recenter events. No threshold relaxation or gameplay change.
  138 adapter and 98 storage assertions pass; Release x64 rebuild succeeds.
  Installed hash 58BB6D105EA936E818DE1D8E759041A5B977B2EFA37114A4CBBECC6279659620.
  Exact positive v22 and original baseline are preserved with guarded restore
  script. Older periodic diagnostics inventoried for later cleanup, not removed
  yet. Next run combines coverage evidence with normal regression checks.

- **2026-09-06 — v23 positive test; residual misses confined to observed handoffs.**
  User reports everything looks good. Retrieved log: 1,472,322 matches, 1,087
  no-credit queries, zero adapter faults and zero matrix/epoch/nonfinite
  rejections. Untruncated miss-frame lists align with native ownership return
  frames or short handoff windows, not steady native gameplay. Supports
  proceeding to cleanup without changing the working correction; not blanket
  Defense/performance validation. Log archived; v23, positive v22 and exact
  baseline hashes checked. No source/deployment/settings change or merge.

- **2026-09-06 — v24 native thumbstick body-turn candidate deployed.**
  Remaining right-stick prompt fighting traced to its separate SendInput path.
  Native mode now delivers existing turn units through Metro +28B280 on the
  native update thread, retaining native sensitivity/ownership/weapon processing.
  Genuine body turn remains separate from HMD cancellation credit. Both-grips
  legacy A/B and combined mouse look delivery remain intact. Bounded mailbox
  prevents accumulated requests; sample timing/turn feel and prompt behavior
  require headset comparison. C++ 98/138/112 assertions and Python 6/10/6 tests
  pass; signatures checked; Release x64 built and installed hash verified:
  F207818B51D2056A98C6F94307573E728FD5426468A8F1FA7A8637993003ECCC.
  Archive native-thumbstick-body-turn-v24-20260906 preserves exact positive v23
  and original fallback, with guarded Restore.ps1. No settings changes or merge.
  Camera-credit correction/Defense timer unchanged; final diagnostics cleanup
  and headset/performance validation remain pending. See Note 34 for caveats.

- **2026-09-07 — v24 turning feel reported good.**
  User reports "Yeah that feels good." Retrieved and archived runtime log:
  native body-input hook installed, native/legacy/native A/B exercised; completed
  native interval has 297038 camera-credit matches, 118 no-credit misses at two
  ownership-return frames, zero adapter faults. Second interval not summarized
  in this snapshot. Installed F207818B... hash reverified. Positive feel is not
  explicit confirmation of prompt-switch elimination or full regression/FPS
  validation. No code/settings/deployment changes; rollbacks retained, no merge.

- **2026-09-07 — v24 no observed input switching; v25 cleanup deployed.**
  User confirms no switching observed, with turning feel already positive.
  Removed temporary camera-credit sampling/counters and active periodic watch,
  gas-mask, aimcull, instance-summary and stereo timing/run diagnostics. Native
  camera/turning/ownership and fail-closed credit checks retained; Defense timer
  behavior untouched. C++ 98/128/112 and Python 6/10/6 checks pass, native signatures
  verified, Release x64 succeeds. Built/installed binary lacks 15 retired report
  strings. Installed SHA256:
  16E94C622B2E897FD3B065C52B208E71AB0A6B4334691E69DE14BD5576E54330.
  Archive native-camera-clean-v25-20260907 preserves exact positive v24 and
  original baseline with guarded restore script. Both-grips A/B unchanged;
  no settings changes or merge. Final headset/Defense/performance validation
  remains pending. See Note 34 for cleanup scope, tests and rollback details.

- **2026-09-07 — v25 Defense-positive; upper-edge visibility/vendor focus follow-up.**
  User reports Defense issues fixed and apparent framerate improvement (not a
  measured FPS-reference comparison). Remaining upper-edge disappearance and
  missing per-gun weapon-vendor focus are now in scope. v25 hash verified and
  retained unchanged. Found native trade_camera_track data and a potential
  projection-coverage mismatch requiring labeled gameplay evidence; old 110-degree
  FOV write is unreachable, despite earlier note. Extended only the bounded
  external read-only camera sampler; no gameplay code/settings/deployment change.

- **2026-09-07 — v26 main-world visibility coverage candidate deployed.**
  Widen only the verified main-world frustum constructor call using both headset
  projections and a roll-independent envelope; preserve native depth clipping,
  shared matrices, FOV/sensitivity and other passes. Native-mode A/B only. This
  targets upper-edge loss; vendor selected-gun framing remains unresolved and
  unchanged. C++ coverage 2893 + adapter 20 checks and prior 98/128/112 checks
  pass; archived signatures verified; Release x64 and binary cleanup check pass.
  Installed SHA256 78DF13EFF55DDA9E6292B98A156FE97FB12E4C46D0EE96DD4C20BBD957784020.
  Exact positive v25 and baseline retained with guarded restore. Wider visibility
  may cost rendering time; actual symptom/performance test pending. No merge.

- **2026-09-07 — v26 top-edge fix confirmed; menu option deferred.**
  User confirms upper-edge disappearance fixed, with a small perceived framerate
  reduction (not a measured FPS-reference comparison). At the user's request,
  note a saved "Expanded visibility" toggle for the future VR-menu revamp,
  proposed default ON. OFF would restore narrower visibility, potentially faster
  but with edge pop-in; native turning/input/Defense fixes must remain independent.
  Documentation only; current behavior, installed DLL and rollbacks unchanged.
  Vendor selected-gun framing remains unresolved. See Note 34. No merge.

- **2026-09-07 — v27 native-only camera build deployed.**
  User requested retiring mouse mode. Native follow starts automatically with
  existing settling/history initialization; removed both-grips mode switching,
  HMD mouse delivery/cursor pinning and thumbstick mouse fallback. Native credit,
  body turning, scripted handoff, Defense timer and v26 visibility preserved.
  Release x64 and C++ 98/128/112/2893/20 checks pass; native-only source/binary
  guards and diagnostic-cleanliness checks pass. Built/installed SHA256:
  80D4B05118529026D9F40C4262696B5D99EC501EA04A29F23C5697E994DB36E5.
  Metro closed during deployment. Archive native-only-camera-v27-20260907 retains
  exact v26 A/B and original fallback, with guarded restore defaulting to v26.
  Cold-start headset validation pending. Vendor framing unchanged; next separate
  investigation. Deferred visibility menu option unchanged. No merge.

- **2026-09-07 — v28 exact native vendor-owner candidate deployed.**
  Traced the trade/customize controller's own camera-track configuration and
  paired activation/deactivation vtable methods (+42DDE0/+42DE3C). A signature-
  and-vtable-guarded hook now feeds that exact owner into the existing authored
  camera handoff, preserving Metro's live selected-gun framing without guessed
  targets, global pitch passthrough, input-E1 or default_true classification.
  First enter/exit logs are bounded one-shot candidate evidence; no hot tracing.
  Mocked adapter 12, native 98/128/112 and visibility 2893/20 checks pass;
  Release x64 and native-only/diagnostic-clean binary guards pass. Installed:
  68496A1509F69326991E8DB1D1F3D2DC35CBC2547E34BCAA52A1137BA7E84A87.
  Metro closed and hashes verified during deployment. Archive native-vendor-
  camera-owner-v28-20260907 preserves exact positive v27, v26 and baseline with
  guarded restore defaulting v27. Headset vendor browse/exit test pending. No merge.

- **2026-09-07 — v28 was inert; v29 late trade-controller install deployed.**
  User saw no vendor change and supplied a non-VR screenshot showing the expected
  dedicated overhead selected-item inspection view across vendor categories.
  Retrieved log proves v28 rejected the trade code/vtable at early startup and
  never observed activation, so it did not test the handoff. v29 silently retries
  the same exact signature/vtable validation from frame update until Metro exposes
  that UI class, then installs once; unavailable/partial states remain fail-closed.
  Mocked late-install coverage rises to 14 checks; existing 98/128/112/2893/20,
  Release x64 and binary guards pass. Installed SHA256:
  78B141AE094C6F1E9CB98E336B2870E630EE15F7EC2950E529969FD53C1CF060.
  Archive native-vendor-camera-late-install-v29-20260907 preserves positive v27,
  rejected inert v28 and baseline, default restore v27. Headset test pending;
  non-VR capture not yet needed. No merge.

- **2026-09-07 — v29 inert; live non-VR reference resolves signature; v30 deployed.**
  Log again showed no vendor install/activation. User supplied the requested
  non-VR overhead vendor state; external read-only capture confirms Metro's
  camera is already positioned overhead and pitched about 54.3 degrees down,
  matching the native state seen under VR. Live method bytes revealed v28/v29
  omitted a leading 0x40 REX prefix hidden by the archived instruction display;
  both relocated vtable targets remain exact. v30 corrects only those guarded
  prologues, retaining late install and exact pointer-matched ownership. Tests
  14/98/128/112/2893/20, Release x64 and binary guards pass. Installed SHA256:
  266696813280737F88ECC702B88DE1B72F475D06D750357AF225B9BA819D9778.
  Archive native-vendor-camera-prefix-v30-20260907 preserves positive v27,
  rejected v29 and baseline with default restore v27. Headset test pending;
  temporary bounded live-byte sampler option remains until cleanup. No merge.

- **2026-09-07 — v30 vendor framing confirmed; clean v31 deployed.**
  User confirms the corrected exact trade-controller hook restores Metro's
  overhead selected-item vendor view. Retrieved runtime evidence shows guarded
  install, native trade activation/deactivation and authored-camera ownership
  acquire/release. Removed only the bounded lifecycle diagnostics and temporary
  `--vendor-owner-code` sampler option; camera/input/visibility/Defense behavior
  is unchanged. Updated stale log wording and added source/binary cleanup guards.
  Release x64, vendor 14, native 98/128/112 and visibility 2893/20 checks pass.
  Clean SHA256: 0B720BFF59EE42496101AE4D3AB0B20260D226EAA94AF5B207F78E2E4CD1C929.
  Archive native-vendor-camera-clean-v31-20260907 preserves exact headset-
  positive v30 (26669681...) as immediate/default rollback, positive v27 and
  original fallback. Metro was closed; installed v30 was verified before
  replacement and installed v31 afterward. Broad regression/performance
  validation remains; no merge.

- **2026-09-07 — bounded town authored-gate diagnostic deployed.**
  User reports that only some towns behave like a scripted scene: vertical
  world motion follows the HMD and native right-stick body turning is disabled.
  The existing clean-v31 runtime log independently shows native camera/player
  ownership acquired shortly after gameplay latched and no later release, but
  it does not distinguish the effector, vendor, or player-performance sources.
  Added behavior-identical bounded source records on transitions and every 600
  frames while stuck, capped at 48 lines. Records include the exact camera and
  viewmodel effector flags, vendor owner, player-performance result, and raw
  `+F78/+F90/+15D0/+15D2` fields. No camera/input/rendering classification or
  timing changed; no stack walk, GPU readback, memory write, or per-frame log.
  Release x64 built with zero errors. Native C++ checks 98/128/112/14/2893/20,
  Python state/lineage/instance/look-gate tests, archived signatures and clean
  native-only/play guards pass. Deployed with Metro closed; installed hash:
  `804B3967AB8E6FD9B12584930997463F421A930673B5C771A454B560D382AB8B`.
  Archive `Builds/town-authored-gate-diagnostic-20260907` preserves the exact
  clean v31 rollback `0B720BFF...`, candidate/PDB/source and prior runtime log.
  Next: visit one affected town long enough to reproduce, exit Metro, then use
  the new source records to narrow the lifetime fix without weakening validated
  Riga, Armory, Defense, or vendor handoffs. No fix or headset-success claim.

- **2026-09-07 — town player-performance/camera separation candidate deployed.**
  The affected-town capture identifies the sole leaking source: no authored
  camera/viewmodel effector or vendor owner was active, while player modes
  remained `(1,1)` for the full run with null explicit owner and zero sentinel.
  This broad pair was originally validated for authored hand/arm preservation,
  then later reused as camera ownership. The candidate retains it only through
  `ScriptedHandPerformanceActive()` and removes it from native HMD/angular,
  upstream camera yield, right-stick body-turn/locomotion and rendered scripted-
  pitch gates. Exact camera effectors and the trade/customize owner are unchanged.
  Temporary source diagnostics were removed. A new source verifier enforces the
  hand-only boundary and rejects their return. Release x64 built with zero errors;
  native 98/128/112/14/2893/20 checks, Python state/lineage/instance/look-gate
  suites, archived signatures and clean native-only/play guards pass. Deployed
  with Metro closed; installed SHA-256:
  `247C3B850DEFC3512F2347F4CD3232192D5F320650C84F0CC2C22EFAD4EF1897`.
  Archive `Builds/town-player-performance-camera-separation-20260907` preserves
  candidate/PDB/source, diagnostic `804B3967...` and clean v31 `0B720BFF...`.
  The prior diagnostic archive now includes the complete affected-town capture.
  Same-town HMD/right-stick validation is next, followed by scripted Riga/Armory/
  Defense and vendor regression checks. No headset-success or merge claim yet.

- **2026-09-07 — first town fix positive but scripted forcing regressed; native-gated candidate deployed.**
  User confirms `247C3B85...` fixes the affected towns, proving the broad `(1,1)`
  state caused their HMD/right-stick suppression. However, removing that state
  from all camera consumers also removed forced looking in real scripted scenes.
  The replacement combines it with Metro's existing verified native look gate:
  player performance owns the camera only while Metro itself refuses player
  look. Persistent `(1,1)` town state can therefore retain authored hand routing
  without suppressing HMD/body input, while a forced-look performance still
  yields upstream/rendered camera ownership. Exact effector and vendor ownership
  are unchanged. The source verifier now permits the raw state only in this
  native-gated camera classifier and hand routing. Release x64 built with zero
  errors; native 98/128/112/14/2893/20 checks, Python state/lineage/instance/
  look-gate suites, archived signatures and clean native-only/play guards pass.
  Deployed with Metro closed; installed SHA-256:
  `1EB208581B06345E66E876E639ABF753858CA9C357B9AFB30683DA18DDD90D3B`.
  Archive `Builds/town-native-look-gated-performance-20260907` preserves this
  candidate/PDB/source, the town-positive/scripted-negative `247C3B85...`, and
  clean v31 `0B720BFF...`. Next headset run must cover the same affected town
  and a known forced-look scene. No headset-success or merge claim yet.

- **2026-09-07 — native-look-gated town/scripted fix headset-validated.**
  User confirms the affected towns remain fixed and various scripted scenes
  retain correct forced-looking behavior. Retrieved runtime evidence verifies
  the installed candidate hash `1EB20858...`, successful native follow/body-turn
  initialization, two authored camera acquire/release pairs, and no native
  failure or fault marker. The accepted distinction is therefore: `(1,1)`
  player performance continues to preserve authored hands, but owns the camera
  only while Metro's verified native look gate is closed. Exact effector/vendor
  camera ownership remains unchanged. The positive runtime log is archived in
  `Builds/town-native-look-gated-performance-20260907`. No source, binary,
  settings or deployment change followed validation. Full-game regression and
  measured performance comparison remain separate; no merge.

- **2026-09-07 — town/scripted-camera branch release closeout.**
  Audited the complete branch after positive validation. The temporary authored-
  gate source sampler and its binary strings are absent; only verifier rejection
  lists retain their names. The accepted ownership distinction is implemented
  directly in native C++ and the Release and installed DLLs both verify as
  `1EB208581B06345E66E876E639ABF753858CA9C357B9AFB30683DA18DDD90D3B`,
  so no learned file, setting, or runtime-generated data needs embedding. Wired
  `verify_town_camera_ownership.ps1` into the standard native-only release guard
  so raw `(1,1)` performance state cannot again become an ungated camera/input
  consumer. Committed the diagnostic, rejected-first-fix, and accepted-candidate
  archive READMEs; DLL/PDB/runtime-log artifacts remain local by design. Full
  retained native tests and release guards pass. Branch is ready for fast-forward
  merge; no DLL rebuild/deployment or settings change was required by closeout.

- **2026-09-07 — late-game separated-hands ownership candidate.**
  Late-game gameplay retained working 6DOF but showed the complete native arms,
  left hand attached to the right-hand viewmodel, and no physical wrist timer.
  This matches the remaining intentional state split after the town fix: raw
  player performance `(1,1)` was native-look-gated for camera/input ownership
  but still directly owned hand routing. Some later chapters retain that pair
  during ordinary controllable play. Scripted hand ownership now applies the
  same verified native-look distinction; exact authored viewmodel effectors
  remain independently authoritative. When Metro allows look, the existing
  12,132/21,762 split path hides sleeve-only ranges, attaches the left hand and
  physical watch to the left controller, and republishes the parent consumed by
  the custom time/filter display. Camera, 6DOF, mesh splits, watch calibration,
  and timer decoding are unchanged. Release x64 built with zero warnings and
  zero errors; retained native optimized checks pass at 98/128/112/14/2893/20,
  all Python native-state/lineage/instance/look-gate tests pass, and release
  guards pass. The exact pre-change `1EB20858...` DLL and candidate are archived
  at `Builds/late-game-separated-hands-native-gate-20260907` with a guarded
  restore script. Candidate and installed SHA-256:
  `5897DB9D09433BC115F51098BB5DAA10F18A917BCF8B624AF29FFFFF4B7C8436`.
  **Rejected in headset:** it made no visible change to the late-game arms,
  hand separation, watch, or timer. This falsifies the ownership-only diagnosis;
  the candidate source and installed DLL were rolled back completely to
  `1EB20858...`. Its run log is preserved in the archive. Do not make another
  behavioral change until a bounded draw diagnostic proves the late-game mesh
  identity and the route it takes.

- **2026-09-07 — late-game hand/arm identity census prepared.**
  The rejected run already shows the recognized 21,252 prologue family and its
  familiar 6,108/2,628/750/60 watch passes, so their presence alone cannot
  identify the visible offending arms. Added a behavior-neutral bounded census
  of unique large single-instance matrix draws plus the watch candidates during
  gameplay. It records draw ranges, shader pair, viewmodel/matrix classification,
  known opening/post-clothing identity, pending instance state, and whether a
  completed left-hand/watch parent is current. It performs no GPU readback and
  changes no draw, suppression, transform, camera, input, watch, or timer route.
  One affected-save run should distinguish a new outfit mesh, a shader/material
  variant of the 21,762 mesh, or an additional duplicate arm submission before
  another behavioral candidate is attempted. Release x64 built with zero
  warnings and zero errors; native-only and town ownership release guards pass.
  Diagnostic and installed SHA-256:
  `5CE511AFE5ED7344F2A547B029E8F9DE6907AD75F80900530E9713FB50042FE3`.
  Archive: `Builds/late-game-hand-identity-census-20260907`, with the exact
  `1EB20858...` pre-change DLL and guarded restore script. One affected-save
  run is pending.

- **2026-09-07 — late-game census result and exact mesh capture prepared.**
  The affected run proves a new 42,516-index skinned arms/hands submission at
  start 1,872,066 / base 564,407 with VS `79BA9F1ECFAF0CA1` and PS
  `E536B13653B5CBDD`. It uses the same clothing shader pair as the earlier
  21,762 mesh but fails that count-specific classifier, so Metro draws it whole.
  The same frame submits the physical 6,108/2,628/750/60 watch family with no
  current completed left-hand parent, directly explaining why the custom timer
  cannot land on the wrist. Added one exact count+shader-qualified capture of
  this mesh's index range, referenced vertex span, b8 palette, and 64-byte native
  instance. It attempts once, performs no rendering change, and will supply the
  disconnected component/range evidence needed to hide sleeves and split both
  hands safely. Release x64 built with zero warnings and zero errors and was
  deployed with SHA-256
  `D00E0E427E815187565801496C290EC53C9222361E9FEE11E62183BEF9A20E17`.
  Archive: `Builds/late-game-hand-mesh-capture-20260907`, preserving both the
  prior census and exact validated rollback DLLs. Another affected-save run is
  required before implementation.

- **2026-09-07 — captured-topology late-game hand split candidate.**
  The exact capture contains 29 disconnected components and proves the new
  42,516-index submission is a full character/upper-body asset rather than the
  earlier standalone combined-hands mesh. Each hand is topologically connected
  to its forearm, so the implementation derives distal triangles by authored
  triangle centroid beyond X +/-0.60. The exact classifier requires index count
  42,516, one instance, VS `79BA9F1ECFAF0CA1`, and PS
  `E536B13653B5CBDD`. In ordinary gameplay the full draw is suppressed and only
  5,046 right-hand plus 5,052 left-hand indices are issued through grouped
  controller/palette routes; all head, torso and arm geometry remains omitted.
  The left group republishes the existing completed wrist/watch matrices, so
  the physical watch and already implemented digital time/timer follow the
  established route without decoder or calibration changes. Positively owned
  scripted performances preserve the complete native draw. A missing instance
  binding suppresses this full-body family rather than exposing arms while the
  normal live-binding recovery runs. The capture/census readback and logging
  hooks are removed. `test_late_game_hand_split.py` verifies source ranges,
  component ownership, cutoff, and exact index totals against the captured
  binary. Release x64 builds with zero warnings/errors; retained optimized
  native checks pass at 98/128/112/14/2893/20, all Python native state/lineage/
  instance/look-gate tests pass, and all release guards pass. Candidate SHA-256:
  `9EAEC1F43D481DA24FE7FD2660A95CBD26837D7D33962AAD42237B144BACF55C`.
  Archive: `Builds/late-game-separated-hands-candidate-20260907`. Headset
  validation of the affected save is still required; no acceptance or commit.

- **2026-09-07 — first captured-topology split rejected; pure-bone v2 prepared.**
  Headset video `2026-09-07 13-09-25.mp4` rejects candidate `9EAEC1F4...`:
  forearms remain visible, both hands remain at the weapon/right controller,
  and long black triangles stretch across the view while watch pieces separate.
  The runtime log identifies the mechanism at frame 1248: the late mesh loaded
  the old embedded gameplay hand pose and evaluated its old geometry landmarks,
  yielding the impossible wrist reference `(-0.84770,-0.01055,-0.68880)`.
  Offline weight analysis further proves the position-cut ranges included
  triangles blended with forearm/body rows 117/120/123/138/141; moving only
  their hand bones created the ribbons. V2 selects all and only triangles whose
  weighted vertices belong exclusively to the late hand chains: 3,603 indices
  on rows 6..54 for the left and 3,603 on rows 63..111 for the right. It adds a
  dedicated late-game left profile derived from the captured 720 hand vertices
  and 41 sleeve-facing wrist vertices, retains the live late-game pose, and no
  longer applies the incompatible embedded earlier-outfit pose. All mixed
  forearm/body triangles remain suppressed. The exact offline verifier proves
  complete pure-hand coverage and rejects any mixed/body triangle. Release x64
  builds with zero warnings/errors; retained native 98/128/112/14/2893/20,
  Python state/lineage/instance/look and release guard suites pass. Candidate
  SHA-256: `C63278EDA38808B84E95A62D2006E6C46EF2B5E444B63E9E950EA119146968F2`.
  Archive: `Builds/late-game-separated-hands-pure-bones-v2-20260907`, including
  the rejected DLL, run log and video contact sheet. Deployed with Metro closed
  and the installed hash verified equal to the candidate. Headset validation
  pending; no acceptance or commit.

- **2026-09-07 — pure-bone v2 rejected; actual-viewmodel isolation prepared.**
  User reports v2 leaves the arms and weapon-bound left hand unchanged; the
  watch plus an unidentified fragment follows the left controller. Because two
  materially different 42,516 triangle selections did not alter the visible
  hands, that outside-viewmodel submission is not their owner. It remains useful
  only for publishing the same-frame native watch parent. Re-reading the first
  affected census identifies the missed candidate: a 17,784-index draw at start
  89,421 / base 15,170, VS `79BA9F1ECFAF0CA1`, PS `BD723CC8D6496169`, and it
  is the only large candidate inside the actual viewmodel pass. Prepared an
  exact isolation build which suppresses only that identity in gameplay and
  captures its index range, referenced vertices, b8 palette, and instance once.
  The 42,516 draw now renders no geometry and only republishes the watch parent,
  eliminating its stray fragments. If the arms/left hand disappear, this one
  run both proves ownership and supplies the topology for the final split; if
  unchanged, the candidate is cleanly falsified. Release x64 builds with zero
  warnings/errors; optimized native 98/128/112 checks and native release guards
  pass. Diagnostic SHA-256:
  `DDA1F8A88500A58B18CE73388E4DBCAA52EE446A10C247C4B9B9E02A11A58E50`.
  Archive: `Builds/late-game-visible-arms-isolation-20260907`. One affected-save
  isolation/capture run is required; no acceptance or commit.

- **2026-09-07 — actual late-game arms identity confirmed; exact hand split deployed.**
  The user confirms diagnostic `DDA1F8A8...` removes both visible arms and
  hands, positively identifying the exact 17,784-index viewmodel draw rather
  than inferring ownership from shader family or clothing state. Its successful
  capture contains exactly four disconnected components and five contiguous
  runs: right sleeve `(0,3285)`, right hand `(3285,2844)`, left sleeve
  `(6129,3285)`, left hand `(9414,5607)`, and the remainder of the same right
  hand `(15021,2763)`. The final route therefore omits both complete sleeve
  components and sends exactly 5,607 indices per hand through the established
  independent controller paths. The left component uses its captured palette
  rows 3..57 and dedicated centre/wrist coefficients, then republishes the
  completed left-hand parent used by the physical watch and custom digital
  time/timer. The separate 42,516 draw remains geometry-free but still prepares
  its proven same-frame watch parent, eliminating the unidentified fragment
  seen in both rejected candidates. The exact 17,784 identity bypasses this
  chapter's persistently asserted whole-viewmodel performance latch; missing
  instance bindings suppress it rather than exposing native arms for a frame.
  `test_late_game_visible_hand_split.py` verifies the captured four-component
  topology, exact shader identity, omitted sleeve ranges, hand ranges, bounds,
  bone families, and equal per-hand totals. Release x64 builds successfully
  with zero errors (eight pre-existing third-party warnings). Native optimized
  suites pass at 98/128/112/14/2893/20, all Python native state/lineage/
  instance/look-gate suites pass, and native-only/town release guards pass.
  Candidate SHA-256:
  `5095301781A02E5C61747274F8BAB1F610F2CC2F7B3F88E897B698D4A09238E9`.
  Archive: `Builds/late-game-visible-hands-split-candidate-20260907`.
  Headset validation of hand placement, hidden sleeves, physical watch, and
  digital timer is still required; no acceptance or commit.

- **2026-09-07 — exact late-game hand split headset-validated.**
  User confirms candidate `50953017...` looks correct in the affected save.
  The captured 17,784-index split is accepted: both sleeves are hidden, hands
  are independently controller-attached, and the left wrist/watch assembly is
  restored. This closes the hand/outfit issue. The next late-game issue is an
  intermittent head-authored laser beam plus native red dot in the same save;
  it is being investigated independently from the accepted hand geometry.

- **2026-09-07 — late-game intermittent laser route diagnostic prepared.**
  The reported beam switching between gun and head plus reappearing native red
  dot matches one shared failure boundary: rejection of the exact 24-index
  player beam prevents both its map-time weapon fold and refresh of the latch
  that suppresses the known 36-index projector and 96-index native point. The
  existing implementation already handles the exact beam and both dot layers,
  so no new mesh guess or behavioral change was made. A bounded transition-only
  trace now records player ownership, predecessor/layout recognition, actual
  weapon folding, beam-base coordinates/radius, and stable predecessor shader
  hashes at the exact beam draw. Release x64 builds with zero warnings/errors;
  the accepted late-game hand topology verifier still passes. Diagnostic and
  installed SHA-256:
  `89A64EDCF75E11A9C17D67186B807E87E61E3764E117ABF7EB0CC364F2D7BC72`.
  Archive: `Builds/late-game-laser-route-diagnostic-20260907`, preserving the
  exact validated hand baseline. One affected-save reproduction is required.

- **2026-09-07 — late-game laser one-frame learning gap fixed and deployed.**
  The affected-save run captured the brief failure at frames 1858, 2021, 2205,
  2685, and 4350. Every event retained positive player ownership, current
  object data, the exact laser layout, and a valid near-eye beam base (radius
  1.10–1.14 m). Only predecessor recognition changed: each newly encountered
  shader pair produced `known=0/folded=0` for exactly one frame, then was learned
  and returned to `known=1/folded=1` on the next frame. That proves the visible
  head jump was the map-time predecessor learning gap, not an ownership-radius,
  hand, weapon, or new-mesh failure. The six exact VS/PS pairs positively seen
  immediately before the exact 24-index player beam are now seeded at startup.
  The established exact-layout + exact-predecessor + near-eye-player gates are
  unchanged, preserving NPC-laser and wall-decal exclusions. The temporary
  route trace and data fields are removed. The offline verifier parses the
  captured failure run, proves all five late misses had this single cause, and
  requires every positively observed pair in the clean source. Release x64
  builds with zero warnings/errors; native optimized suites pass at
  98/128/112/14/2893/20, Python state/lineage/instance/look-gate tests pass,
  both late-game hand and laser-route verifiers pass, and release guards pass.
  Candidate and installed SHA-256:
  `52D0D825846759ECAD9CED6A0A59A0C60C7C2BB5BAA044FC79EE7975370917B1`.
  Archive: `Builds/late-game-laser-predecessor-seeds-candidate-20260907`.
  Headset validation of continuous gun attachment and native-dot suppression is
  pending; no acceptance or commit.

- **2026-09-07 — predecessor seeding rejected; exact player beam now fails closed.**
  User reports `52D0D825...` leaves the same intermittent head-attached beam and
  native red point, so seeding the six observed render contexts is insufficient
  and is not considered a fix. The prior trace nevertheless proves the unsafe
  boundary directly: the exact positively owned player beam was submitted from
  Metro's camera whenever its validated map-time weapon fold was absent. The
  exact 24-index player-beam draw now refreshes native-dot suppression and
  learns its layout/predecessor as before, but suppresses that individual frame
  when `sLastObjectWeaponFolded` is false. It therefore cannot render from the
  head during first-layout discovery or any future unseeded predecessor frame;
  the following frame becomes eligible normally. Exact player ownership remains
  required, so NPC beams are not suppressed, and no broad layout-only fold is
  introduced that could move wall decals. The custom dot also remains fail-
  closed when no fresh corrected finite beam pose exists. Headset validation is
  required; no acceptance or commit.

  Release x64 builds with zero warnings/errors. Native optimized suites pass at
  98/128/112/14/2893/20, Python state/lineage/instance/look-gate tests pass,
  both late-game hand and laser-route verifiers pass, and release guards pass.
  Fail-closed candidate SHA-256:
  `1EAC0D3AA14832F5691F1BCCA34498B0F4C774B401B8B92A4238C990A440F1F3`.
  Archive: `Builds/late-game-laser-fail-closed-candidate-20260907`, including
  the rejected seed build and the evidence log. Deployed with Metro closed and
  installed hash verified. Headset validation remains pending.

- **2026-09-07 — fail-closed candidate rejected; known laser layers isolated.**
  User reports `1EAC0D3A...` leaves the same issue, so neither predecessor
  seeding nor suppressing an unfolded exact beam addresses the visible late-save
  artifact. A direct blame/diff audit against headset-validated commits
  `7c43482` and `562da8e` confirms the accepted map-time correction, accumulated
  stable predecessor hashes, near-eye player gate, finite endpoint, 36-index
  projector replacement, and 96-index native-point suppression remain intact;
  later hand/camera work did not overwrite them. The working source is restored
  to that accepted dynamic predecessor behavior. A diagnostic now suppresses
  all four known player-laser layers together: exact 24-index beam, custom
  stereo marker, 36-index projector, and 96-index native point. If anything
  described as the beam/red dot remains, the late save contains an additional
  draw identity and the prior route must not be changed again. Release x64
  builds with zero warnings/errors; late-game hand topology and native release
  guards pass. Isolation and installed SHA-256:
  `A783EE91A75E4CD46CC76D7BC7E85BA242D451FC4219A94C8F269874D066319D`.
  Archive: `Builds/late-game-known-laser-layer-isolation-20260907`. One visual
  isolation run in the affected save is required; no acceptance or commit.

- **2026-09-07 — isolation proves an additional late-game native red-dot layer.**
  With diagnostic `A783EE91...`, the user reports the laser beam is gone but
  the red dot remains. Therefore the visible beam is conclusively the accepted
  exact 24-index identity, while the surviving red point is neither the custom
  marker nor the known 36-index projector or 96-index native point. Restored
  the accepted beam route, kept the custom marker disabled for an unambiguous
  native-point capture, and added a one-shot mono render-target history that
  starts automatically immediately after exact player-beam ownership. This is
  the same contribution-boundary method that identified the prior 96-index
  layer, without requiring timed F8 input. Hunting mode is temporarily enabled
  in the game ini solely to select the frame-analysis context; the exact prior
  ini is archived for restoration. Release x64 builds with zero warnings/errors
  and the late-game hand topology verifier passes. Capture/installed SHA-256:
  `226AD5D798CF6278F7346B50922F0491424E400CC0AC43CF9A295D326D45670C`.
  Archive: `Builds/late-game-red-dot-frame-analysis-20260907`. One launch of the
  affected save with the red dot visible should capture its first contribution.

- **2026-09-07 — late red-dot capture proves the producer precedes the beam.**
  The automatic capture completed at frame 1208 in
  `FrameAnalysis-LateRedDot-20260907-143849-003`; the temporary game
  `hunting=1` setting was immediately restored to `hunting=0`. Its first render
  target, dumped immediately after the exact 24-index beam, already contains
  the unwanted native impact point. Consequently no draw after the beam can be
  its producer, and suppressing another post-beam light pass would be another
  unsupported guess. The custom stereo marker is restored and the automatic
  frame capture is removed. A metadata-only diagnostic now records every draw
  in the current frame and writes `late_red_dot_prebeam_draws.txt` when the
  exact beam arrives, including draw kind/count/range, shader pair, render
  target/viewport, and latest object translation. It does not skip, move, or
  otherwise alter any draw. Release x64 builds with zero warnings/errors and
  is deployed with matching SHA-256:
  `2266D0A54B155FCB2C6D704DC69F4E18A348CD2F7BDB79A4A0D77B45AE533BE6`.
  One launch of the affected save is required to collect the pre-beam producer
  sequence; no acceptance or commit.

- **2026-09-07 — full-frame capture identifies late-game native red projector.**
  The pre-beam trace recorded 838 draws and confirmed that the established
  96-index point (`VS 98CD...`, `PS 67F4...`) is reached and suppressed, while
  several other deferred-light variants precede the beam. A second automatic
  capture started at the first draw of the following frame. Render-target
  history is conclusive: event 853 has no red pixels; event 854 adds the entire
  15-by-5-pixel red point in one draw. That draw is `DrawIndexed(36)`, VS
  `98CD6A5E2980BAE1`, PS `A9DE870980578818`. It is the late-game projector
  variant whose cb-light payload falls outside the older structural classifier.
  The final candidate suppresses this exact pair only while a positively owned
  player beam is fresh, invokes the established finite controller-aligned
  stereo replacement dot, and leaves the accepted dynamic beam-predecessor and
  independent 96-index suppression paths intact. All capture/isolation code is
  removed and the game configuration is restored to `hunting=0`.

  Release x64 builds with zero warnings/errors. Native optimized suites pass at
  98/128/112/14/2893/20; Python camera/state/lineage/instance/look-gate tests,
  late-game hand topology, and updated laser-route verification pass; native
  release guards pass. Candidate SHA-256:
  `BAD2540FBBEB0C7EA9E53995F694031B343CB53BA0F18C69D545F15A45798859`.
  Archive: `Builds/late-game-native-red-dot-projector-candidate-20260907`,
  including the before/after render targets and capture logs. Headset validation
  is required; no acceptance or commit.

- **2026-09-07 — late native dot validated; intermittent beam moved to exact-draw correction.**
  The user confirms the `BAD2540F...` candidate removes the native red dot, so
  the exact late-game `DrawIndexed(36)` projector suppression is retained as an
  accepted visual fix. The original laser beam can still attach to the head
  intermittently. The surviving failure is therefore isolated from both native
  dot layers and from the custom replacement marker.

  The player-beam transform no longer relies on the shader bound immediately
  before Metro maps its object constant buffer. At the exact 24-index beam draw,
  the candidate rebuilds both stored eye copies directly from that draw's world
  matrix, the current per-eye view/projection matrices, the weapon affine, and
  the canonical barrel ray. The same exact layout and near-eye player-pose gate
  remain in force, preserving NPC beams. If a map-time player classification
  cannot be rebuilt at the exact draw, that single unsafe frame is omitted
  instead of being submitted with a head transform. The rebuilt right-eye copy
  remains available to the established twin-pass path.

  Release x64 builds with zero warnings/errors. Native optimized suites pass at
  98/128/112/14/2893/20; Python camera/state/lineage/instance/look-gate tests,
  late-game hand topology, and laser-route verification pass; native release
  guards pass. Candidate SHA-256:
  `FC06C760575B3272718E17E0E442463C88E8022C74904FB2973D1CD161E4F4FB`.
  Archive: `Builds/late-game-exact-beam-draw-rebuild-candidate-20260907`.
  Headset validation of continuous controller attachment is required; no commit.

- **2026-09-07 — exact-draw rebuild rejected; beam-route trace deployed.**
  The user reports `FC06C760...` leaves the same intermittent head-attached
  laser, while the previously isolated late native red dot remains gone. The
  exact-draw rebuild is therefore not accepted: applying the validated
  controller/barrel correction again after the exact beam shader is known did
  not change the failure. Direct comparison with accepted commits `7c43482`
  and `562da8e` confirms their prologue beam identity, player proximity gate,
  weapon fold, and barrel-ray alignment are still present.

  An observation-only trace now records every exact 24-index beam submission:
  map-time ownership, exact rebuild result, cb0 binding and both stored-eye byte
  sizes, raw and pre-rebuild stored beam origins, controller affine origin,
  predecessor hashes, layout recognition, prior fold state, ownership age, and
  the active batch/single-pass/twin mode. This distinguishes an unclassified
  duplicate player draw from a correctly corrected draw that is later replayed
  through a different stereo path. It does not suppress or move any additional
  geometry. Release x64 builds with zero warnings/errors; focused laser and
  late-game hand verifiers pass. Diagnostic and installed SHA-256:
  `D23EEE5F736B2CE92448C527BB3D03CB2D6BCBAC59D75F96E83AEA9B5B4E50C6`.
  Archive: `Builds/late-game-laser-exact-draw-trace-20260907`. One affected-save
  reproduction is required; no acceptance or commit.

- **2026-09-07 — trace proves late animation crosses player-beam radius gate.**
  The user reproduced the jump and clarified that both head and right-controller
  motion affected the displaced beam. The exact-draw trace captured 3,169 beam
  frames. Of those, 3,155 were positively owned and rebuilt; exactly 14 failed
  both map-time and exact-draw ownership and were submitted with Metro's
  camera-relative matrices. Every other discriminator remained identical:
  exact beam shader/count, input layout, predecessor pair, bound 208-byte cb0
  with both eye snapshots, available weapon affine, and single-pass/twin state.

  The fourteen misses are precisely the samples whose raw beam origin exceeds
  the prologue-derived `1.25 m` player radius. The late weapon animation reaches
  a measured maximum of `1.319541 m`; samples immediately below `1.25 m` resume
  correct ownership. The player radius is therefore widened only to `1.35 m`,
  providing about three centimetres of measured headroom while retaining the
  exact beam identity and a near-eye envelope well short of room-scale NPC
  origins. The trace is removed from gameplay source. The validated late native
  red-dot suppression and late-game hand/watch fixes remain unchanged.

  Release x64 builds with zero warnings/errors. Native optimized suites pass at
  98/128/112/14/2893/20; Python camera/state/lineage/instance/look-gate tests,
  late-game hand topology, and laser-route verification pass; native release
  guards pass. Candidate and installed SHA-256:
  `9F7F481A9CA7F45A34784FE4B0A6FE7933D41AD7E28FCD1CAD7A83DD97890F30`.
  Archive: `Builds/late-game-laser-135cm-player-envelope-candidate-20260907`.
  Headset validation is required; no acceptance or commit.

- **2026-09-07 — late beam remains gun-based; camera-derived range removed.**
  The `9F7F481A...` envelope candidate still shows the issue. The user's more
  precise observation is decisive: the beam origin remains attached to the gun
  and controller motion still steers it, but head motion changes the beam and
  can move it so far that it only appears to jump to the headset. This rejects
  complete HMD ownership as the description of the main defect. The measured
  1.35 m envelope remains justified for the fourteen real ownership misses, but
  those misses were not the persistent visual behavior.

  The remaining camera-owned degree of freedom is the Z-axis magnitude. The
  accepted prologue correction replaced Metro's camera-authored direction with
  the controller/barrel direction but intentionally retained Metro's finite hit
  range. The shooting investigation independently proved that this range is
  measured along Metro's camera ray and is not a valid range for a relocated
  barrel ray. At surface boundaries, head motion can therefore change the
  length dramatically even though the corrected base and direction stay on the
  gun. The new candidate replaces that native magnitude with a 60 m controller-
  ray reach; the already-bound scene depth buffer clips the visible beam at
  geometry. No camera-derived direction or range remains in the corrected ray.
  Native dot suppression and the hand/watch fixes are unchanged.

  Release x64 builds with zero warnings/errors. Native optimized suites pass at
  98/128/112/14/2893/20; Python camera/state/lineage/instance/look-gate tests,
  late-game hand topology, and laser-route verification pass; native release
  guards pass. Candidate and installed SHA-256:
  `A407AD3AACFEEF85A8ECA7F8EE50BAD6ACCAB536D7809E477A143ECE795EBFB4`.
  Archive: `Builds/late-game-laser-controller-ray-reach-candidate-20260907`.
  Headset validation must confirm both head independence and correct scene-depth
  termination; no acceptance or commit.

- **2026-09-07 — exact beam now uses a rigid muzzle and temporal player lineage.**
  The `A407AD3A...` test confirmed that the 60 m controller ray plus scene-depth
  clipping fixes the beam extending beyond walls, but head motion still moved
  the beam. It also exposed the old NPC regression: an NPC laser close to the
  player moved with the right controller. This proves the remaining problem is
  not the ray length and that the historical “ownership” fix was only a spatial
  heuristic. The shared player/NPC beam draw was still accepted solely when its
  origin entered the 1.35 m near-eye sphere.

  The affected-save exact-draw trace supplies the missing transform evidence.
  Across the camera sweep, Metro's raw player-beam origin moved from roughly
  `(-0.21,-0.65,0.93)` to `(0.15,-0.21,1.07)` while the cached visible-weapon
  affine origin stayed near `(-0.04,0.17,-0.33)`. Thus the controller fold kept
  reusing a camera-authored base even after direction and range were replaced.
  The exact draw now captures each eye's native muzzle once per continuous laser
  run and folds that fixed point through the current visible-weapon affine. Head
  motion can no longer rewrite the base, while controller motion still moves it
  with the gun.

  Laser correction is removed from the ambiguous map-time particle route.
  Exact-draw ownership begins inside the measured player envelope, then requires
  continuity with the previously confirmed raw player muzzle (35 cm maximum
  inter-frame travel, versus under 10 cm measured in the trace). A nearby NPC
  beam sharing the same 24-index shaders therefore retains Metro's world
  transform instead of borrowing the controller merely due to proximity. A gap
  longer than two frames resets acquisition and the per-eye muzzle anchors.

  Release x64 builds with zero warnings/errors. Native optimized suites pass at
  112/98/128/14/2893/20; all Python camera sync/state/lineage/instance/look-gate
  tests, late-game hand topology, laser-route verification, and native release
  guards pass. Candidate and installed SHA-256:
  `F3655C0D6D1BDC0B451978DAE6977465A76936C75ED9231687C567D3EDB61380`.
  Archive: `Builds/late-game-laser-rigid-muzzle-lineage-candidate-20260907`.
  Headset validation must confirm that the player beam remains rigid on the gun
  during head motion, still follows the right controller, stops at walls, and a
  nearby NPC laser remains attached to its NPC. No acceptance or commit yet.

  **Rejected in headset:** the rigid-muzzle/temporal-lineage candidate detached
  the player laser from the gun and did not remove its head response. The exact
  candidate was immediately rolled back in the game directory to the prior
  wall-clipped `A407AD3A...` binary. Its source experiment was also removed;
  none of the rigid-muzzle or temporal-lineage behavior remains active.

- **2026-09-07 — player/NPC exact-beam transform correlation trace.** The latest
  rejection rules out another inferred base anchor. The earlier prologue status
  described its accepted result as “substantially more stable,” not proof of
  perfect head independence, and later native-camera work changed the frames
  feeding the current view. An observation-only trace is therefore installed on
  the restored `A407AD3A...` behavior. For every exact 24-index beam draw it
  records draw order, both ownership decisions, both raw eye origins, rebuilt
  base and axis, visible-weapon affine, canonical reticle direction, controller
  position relative to the head, head translation, and full head rotation.
  Capturing both the player beam and a nearby NPC beam while independently
  moving head and controller will identify the actual borrowed component and
  whether draw order provides stable player lineage. The trace does not alter
  beam, dot, hand, watch, or camera behavior.

  Release x64 builds with zero warnings/errors. Diagnostic and installed
  SHA-256: `8562582F8B12A913CF23312B76A2C9C7F785BA1F8DE503D03E9D74A43F3F4CD5`.
  Archive: `Builds/late-game-player-npc-laser-transform-trace-20260907`.
  One short affected-save run is required; no acceptance or commit.

  **Trace result, corrected after the follow-up headset A/B:** the comparison
  level had no player-beam head-motion defect, but nearby NPC lasers visibly
  followed the right controller. The run captured 8,000 exact beam submissions:
  1,076 draws were positively owned and rebuilt, while 6,924 were rejected
  (`owned=0/0`, `rebuilt=0/0`). The first interpretation labelled the accepted
  interval as the player beam and the rejected 5–6 m draws as NPC beams. The
  per-projector light-source gate based on that interpretation did not change
  the visible NPC attachment, disproving the label. The accepted interval is the
  reproduced NPC approaching the player: its origin converges from 0.812 m and
  remains 0.639–0.680 m from the eye, squarely inside the expanded 1.35 m
  sphere. The rejected draws are the same NPC lasers while farther away.

- **2026-09-07 — NPC laser-light ownership restored for late dot variants.**
  The remaining shared controller route was downstream of the correctly
  rejected beam: the new 36-index A9DE projector suppression and established
  96-index point suppression used only a frame-global `freshPlayerLaser` latch.
  While the player laser was active, a later NPC light draw with the same exact
  shader/count could therefore be suppressed or replaced by the player's
  controller-authored custom marker even though its beam was left native.

  Both exact late light-layer routes now additionally require their own current
  PS cb12 light source to be within the established 1.05 m player-projector
  envelope. This is the per-projector ownership check used by the earlier
  prologue fix. NPC light sources outside that envelope retain their native
  projector and point layers; player layers still receive the validated native
  red-dot suppression and custom finite marker. The 60 m beam reach and scene-
  depth wall clipping are unchanged. The transform trace is removed.

  Release x64 builds with zero warnings/errors. Native optimized suites pass at
  112/98/128/14/2893/20; all Python camera sync/state/lineage/instance/look-gate
  tests, late-game hand topology, laser-route verification, and native release
  guards pass. Candidate and installed SHA-256:
  `4177D290C472A6A224086D831BF86FC28F963EDCCB4513E1436D8B22796D179F`.
  Archive: `Builds/late-game-npc-laser-light-ownership-candidate-20260907`.
  Headset validation should first confirm NPC lasers no longer follow the right
  controller and the player's native red dot remains absent. Then return to the
  original level/save for the separate player-beam head-motion investigation.
  No acceptance or commit yet.

- **2026-09-07 — measured player/NPC beam-shell candidate.** The failed light
  ownership A/B establishes that the remaining symptom is the exact beam line,
  and correcting the trace labels exposes why the earlier prologue rule regressed
  at close range: the 1.35 m maximum-radius test admits both owners. The two
  labelled captures separate cleanly by full origin distance. Across 3,169
  affected-save player frames, the raw player-beam origin stayed
  `1.093–1.320 m` from the eye. Across all 1,076 wrongly accepted nearby-NPC
  frames, the raw NPC origin stayed `0.639–0.812 m`. Ownership is now an annulus
  of `0.95–1.35 m`, leaving over 14 cm of measured margin on each side of the
  lower boundary while preserving the late player animation's measured upper
  requirement. Exact shader/layout identity, both-eye pose agreement, dynamic
  predecessor learning, the 60 m controller ray, and per-projector light gates
  remain required. The rejected rigid-muzzle/temporal-lineage experiment remains
  absent.

  Release x64 builds with zero warnings/errors. Native optimized suites pass at
  98/112/128/14/2893/20; all Python camera sync/state/lineage/instance/look-gate
  tests, late-game hand topology, laser-route verification, and native-only/town
  release guards pass. Candidate SHA-256:
  `E7006E919C9764C4901828A111B4352F3B252E911A1E8F6D3278C548E7E12043`.
  Archive: `Builds/late-game-player-npc-laser-shell-candidate-20260907`.
  The first follow-up report of "same issue" was made before this candidate was
  deployed: hash verification showed the game directory still contained the
  preceding `4177D290...` light-gate build. Once Metro closed, the shell
  candidate was deployed with matching source/destination SHA-256
  `E7006E919C9764C4901828A111B4352F3B252E911A1E8F6D3278C548E7E12043`.
  The previous installed DLL is preserved as `d3d11.pre-change.dll` in the
  candidate archive. Headset validation is now required; no acceptance or
  commit yet.

  **Rejected in headset:** the NPC laser still followed the right controller
  with the measured beam shell installed. The user added a decisive condition:
  their equipped gun does not need a laser sight; the NPC attachment occurs with
  any player weapon. Therefore a nearby NPC effect may be falsely publishing its
  own `freshPlayerLaser` state, and player-equipment presence cannot be inferred
  from that latch.

- **2026-09-07 — NPC laser component trace.** Installed an observation-only
  trace covering the three independently submitted pieces in the same run: each
  exact 24-index beam's two raw origins, shell decisions, rebuild decisions and
  predecessor hashes; the 36-index captured deferred-light route's cb12 source,
  captured-light verdict and acceptance; and the late 36/96-index light routes'
  cb12 sources and acceptance. A reproduction while holding a gun with no laser
  makes every beam entry definitively NPC-owned and removes the prior visual
  labelling ambiguity. Rendering behavior is otherwise unchanged. Focused hand
  and laser verifiers pass; Release x64 builds with zero warnings/errors.
  Diagnostic and installed SHA-256:
  `B5260B78D0188809E0AC18F3D926D01FF48751F7C3064C0FFB84866C908DCED0`.
  Archive: `Builds/late-game-npc-laser-component-trace-20260907`. One short
  no-player-laser reproduction is required; no acceptance or commit.

- **NPC-only capture result and world-history candidate.** The completed
  non-laser-player-gun run contains 1,282 beam submissions: 374 were rebuilt
  for both eyes, 900 rejected for both, and eight straddled an eye boundary
  (five left-only, three right-only). All 1,388 logged light checks rejected.
  This directly proves NPC beam misclassification. Previous claims that the
  beam route was ruled out, or that unlabeled earlier captures proved separate
  player/NPC distance bands, were unsupported and must not guide future fixes.
  Evidence: `Builds/late-game-npc-laser-component-trace-20260907/non-laser-gun-runtime.log`.

  Candidate adds bounded NPC world-position history seeded when an exact beam
  is beyond 1.5m. Matching world origins within 35cm retain NPC classification
  while approaching, with a two-frame expiry. Both eyes are rebuilt with native
  world/view matrices for these NPC draws, undoing an earlier generic particle
  fold. Continuous laser folding at map time is disabled; the exact draw makes
  the decision. The rejected minimum-radius restriction is removed. No muzzle
  anchor is frozen. This is still a heuristic: initial loading beside an NPC,
  gaps beyond two frames, rapid NPC movement and close overlapping origins need
  further validation. Diagnostic logging remains for the next run.

  The actual C++ history tests pass for approach, NPC motion, simultaneous beams,
  expiry, frame reset and invalid data. Release build, focused hand/laser checks
  and native release guards pass. Installed candidate SHA-256:
  `D83E82309C6C067FE301262263AAE0DDEFD38E3D5F0B9B6BCA285A72E69B637C`.
  Archive: `Builds/npc-laser-world-history-candidate-20260908` (folder label).
  Headset acceptance is pending; no commit.

  **Headset validated:** user confirmed "Yeah, that fixed it" after testing
  the NPC world-history candidate. NPC lasers now retain their owner when the
  player approaches with any gun. Removed the temporary component/light/history
  logging without changing the validated classification or transforms. Actual
  C++ history tests, focused laser/hand checks, release guards and Release x64
  build pass. Clean installed SHA-256:
  `356DACF8A228B6C25A6053691A77922466FFAD7C92B1427857E7578D09496010`.
  Archive: `Builds/npc-laser-world-history-validated-clean`, including the
  headset-validated diagnostic DLL and runtime log. The separate player laser
  response to head movement in the other level still requires returning to
  that level; it is not claimed fixed by this NPC validation. No commit yet.

- **Player laser save-load/doorway reproduction.** User confirmed the player
  laser responds to head motion immediately after loading this save. Walking
  through an open doorway fixes it; returning through the doorway leaves it
  correct. Reloading the same save reliably restores the defect. This identifies
  a repeatable state transition, but does not yet identify the faulty cache.

  Installed observation-only load/doorway trace on the validated NPC baseline.
  Every third beam frame (bounded to 12,000 samples) records the original 52-float
  object packet before camera synchronization, native view, eye view/correction,
  complete weapon affine, its producer mesh/left-hand/right-hand flags and frame,
  head/controller pose, submitted beam WV and NPC/rebuild decisions. No render
  logic changed. Release build and focused hand/laser/history checks plus native
  release guards pass. Installed SHA-256:
  `A6C154FC09795509E546D68D34A53FAC60C96F1AB1EFF3C850FB51F7126764BB`.
  Archive: `Builds/laser-save-load-doorway-trace`, including pre-change clean DLL.
  Needed capture: reload, hold gun still and move head before doorway, walk
  through, repeat head movement, return and repeat, then exit. No fix claimed.

- **2026-09-08 — direction-sensitive laser and shared affine contamination.**
  User corrected the doorway hypothesis: thumbstick turning can reproduce the
  issue beyond the doorway. They also report the head-attached native dot and
  beam through walls have returned. Capture contains 2,615 sampled beam frames,
  all rebuilt and none rejected by NPC history. Of these, 792 consume weapon
  parameters last published by the LEFT 42,516-index body/hand draw; 1,823 consume
  parameters from the 36-index weapon attachment. Thus the shared weapon-effects
  cache is concretely overwritten by an independently positioned left hand.

  Both CPU and GPU publication sites now require !leftHand && !rightHandOnly.
  Hand geometry still receives its own transform, but cannot overwrite the
  affine consumed by beam and muzzle effects. This corrects a proven defect;
  headset behavior is still pending. Kept load trace to verify producer identity
  and added bounded depth-state and native-light suppression-source diagnostics
  to investigate the reported wall/dot regressions. Neither regression is yet
  claimed fixed. Focused hand/laser/history tests, release guards and Release
  x64 build pass. Installed SHA-256:
  `B219132229BA89B4FBD9803E5F9014AE9A05BDB5A08EA239F11ACBA41854020F`.
  Archive: `Builds/laser-weapon-only-affine-20260908`, including the failure log
  and previous DLL. No commit.

- **2026-09-08 — head-dot and wall regression candidate.** User could no longer
  reproduce the finicky movement consistently and elected to tolerate it; the
  native head dot and wall penetration remain the requested targets. Latest
  B219 run records 2,977 beam depth samples with depth enabled, LESS_EQUAL and
  full viewport range. Both exact native light variants reject every sampled
  player source (2,971 each), with measured source radius about 1.09-1.13m,
  outside the 1.05m gate. Light xyz matches the native beam WV translation.

  Exact late light suppression now matches the last successfully rebuilt
  player's native world origin, transformed into the current native camera,
  with 20cm tolerance, two-frame freshness and a 1.35m outer envelope. It does
  not merely widen a proximity-only NPC classifier. Validated NPC beam history
  and hand routing are unchanged. Headset confirmation of light ownership is
  pending; load/depth/light diagnostics remain enabled.

  Removed the forced 60m beam extension and restored finite native range while
  retaining controller direction. Previous claims that raster depth stops the
  entire ray at its first wall were incorrect: it only rejects individual hidden
  fragments. This conservative rollback still uses camera-derived range, NOT a
  controller-origin collision query, and cannot guarantee correct intersections
  for every controller pose. Custom marker now rejects endpoints behind a nearer
  scene-position sample instead of unconditionally overlaying that surface.

  Release build, focused laser/hand checks, actual NPC-history C++ tests and
  native release guards pass. Installed and archived DLL hashes match:
  `BD70F479159A9062FC01B227BE415E82BAABA364A3817F11A636B9FB30CA6A1A`.
  Archive `Builds/laser-dot-finite-range-20260908` preserves previous DLL and
  failing runtime log. Candidate needs headset validation; no commit.

  **Headset confirmed:** user reports "OK that seemed to have fixed that"
  for the head dot and wall penetration. Preserve BD70F479 as accepted baseline.

- **2026-09-08 — intermittent flamethrower flame routing investigation.**
  User reports fire switches between the flamethrower tip and head. No specific
  flamethrower identity was found in the notes. Earlier muzzle companion-tail
  investigation identified an omitted exact shader/instance signature, while
  current generic map-time particle folding still depends on a preceding-draw
  marker and distance. Neither is yet proven responsible for the flamethrower.
  Installed observation-only small-draw signature/ownership trace during the
  existing firing/tail window (12,000 entries maximum, every other frame), and
  re-enabled bounded map-time particle gate trace (2,400 entries) to correlate
  proximity and predecessor decisions. No transforms/classifiers changed.
  Release build and focused hand/laser/history tests pass; diff check passes.
  Archive: `Builds/flamethrower-route-trace-20260908`, including accepted dot/wall
  DLL and its runtime log. Installed/source build DLL SHA-256 matches:
  `898327DBC2A9B36AFCD7F9D613D95234B05B3E593DF345FABE5FCAAB0B153F70`.
  Next: restart, reproduce flamethrower switching briefly, release trigger and
  exit for log analysis. This is a diagnostic, not a flamethrower fix. No commit.

- **2026-09-08 — first flamethrower trace insufficient; targeted repeat.**
  First trace saturated all 12,000 entries between frames 21422-21574. Many
  non-instanced screen draws consumed the limit. Recognized E1FDE/FB0FCA
  one-instance companion appears with muzzle=1/folded=0; numerous other shared
  particle families appear, but this does NOT identify the visible flame or
  prove the intermittent failure. Map trace also saturated early. No speculative
  shader addition or radius widening was made.
  V2 traces only six-index instanced batches every 30 frames, armed on first
  firing and continuing through idle for comparison, with 24,000 entry cap.
  Logs whether the actual bound cb0 matches tracked stereo data and its W/WV
  translations. Map gate trace is sampled every 30 frames with 12,000 entry cap.
  Release build and focused laser/hand/history checks pass. Archive:
  `Builds/flamethrower-particle-trace-v2-20260908`, including first runtime log
  and previous diagnostic DLL. V2 installed with matching archive/game hash.
  Needed: short burst, pause, reproduce switching, pause, exit. Rendering remains
  unchanged and flamethrower fix is still pending. No commit.

- **2026-09-08 — flamethrower visual hypothesis corrected by user.** User says
  shoot/pause is impractical and the flame may stay head-authored throughout;
  apparent gun attachment may simply occur when gun and gaze align. Do not
  treat intermittent switching as proven. V2 capture spans frames 1530-3870;
  archived as `Builds/flamethrower-particle-trace-v2-20260908/second-flame-trace.log`.
  Firing samples include active input with LastShotFrame still sentinel (the
  existing muzzle gate includes input, so absent projectile callbacks alone
  do not explain the failure). E1FDE/FB0FCA single-instance is recognized;
  E1FDE/252FCE and other shared particle batches remain unclassified. Some use
  identity world transforms with world-space particles, others object origins.
  Therefore draw signatures alone cannot safely identify the visible flame.
  Mesh 17784 also publishes effects cache with flags=0 via the combat right-hand
  split (not the independent-right-hand route); this is an observation, not proof
  of a wrong affine. No rendering edits made from these ambiguous observations.
  Need visual evidence with sustained fire and gun pointed away from gaze, not
  shoot/pause or subjective reporting of exact transition times. Current installed
  V2 diagnostic remains 4DCBDA35; accepted dot/wall rollback remains archived.

- **2026-09-08 — Video Project 72 reviewed; automatic flame draw capture.**
  User confirms damage originates at flamethrower tip and only the visual fire
  is wrong. Reviewed the 10.8s video via 24 extracted frames (script
  `Tools/review_flamethrower_video72.py`, output `Builds/flamethrower-video72`).
  The plume is visibly separated from the barrel during gun motion, supporting
  head-authored visuals; damage correctness is the user's gameplay observation.
  Do not change projectile/damage/aiming code. Video alone cannot identify the
  shader/particle buffer responsible.

  Enabled established automatic muzzle frame-analysis machinery for sustained
  flamethrower firing after 30 frames, independent of LastShotFrame. It captures
  once per process and dumps RT/CB/VB/IB for exact plume identification, under
  `FrameAnalysis-Flamethrower-*` in game directory. Installed hunting=1 for capture;
  MUST restore hunting=0 and disable capture after evidence is collected. No
  effect transforms/classifiers changed. Build and focused hand/laser/history
  checks pass. Archive `Builds/flamethrower-auto-frame-20260908` preserves prior
  DLL, runtime log, INI and capture source. Game/archive DLL hashes match.
  User only needs restart and normal sustained firing; capture may hitch.
  Flamethrower fix is pending, not claimed complete. No commit.

- **2026-09-08 — captured flame plume larger than muzzle classifier cap.**
  Automatic capture succeeded at frame 1438, directory
  `<METRO_INSTALL>/FrameAnalysis-Flamethrower-20260908-130446-823`.
  Reviewed RT history with `Tools/review_flame_capture.py`; images in
  `Builds/flamethrower-capture-review`. Draws 593/595 add the flame plume,
  VS 2403427473C35A13 / PS 74E951E588F5E7C3, 46 instances each; draw 615
  is a 26-instance continuation. They use identity W and world-space particle
  positions; the map-time object proximity test cannot identify the emitter.
  Current muzzle classifier rejects all batches over 16 instances. Same shader
  has a 200-instance ambient batch at 623, so blindly admitting the PS is unsafe.

  Candidate admits that exact pair at 17-128 instances ONLY with fresh exact
  flamethrower body (40740 indices, VS 79BA9F1ECFAF0CA1 / PS 6597099FD3B895DB,
  captured draw 54), and existing muzzle firing/tail guard. Uses existing live
  and second-eye replay correction. It is still a batch-size/weapon heuristic:
  small startup/fade batches, >128 particles and same-size ambient fire while
  holding this weapon are not proven handled. No per-particle emitter ownership
  has yet been recovered. Do not claim all ambient exclusion or complete fix.
  Aiming/damage untouched. Automatic capture disabled and game INI hunting=0
  restored. Bounded flame logging remains for candidate validation.

  Release build, focused flame-source guards, hand/laser guards and actual NPC
  history tests pass. Candidate deployed with matching archive/game hashes.
  Archive `Builds/flamethrower-large-batch-candidate-20260908` preserves diagnostic
  DLL and capture runtime log. Needs headset check during sustained firing and
  startup/fade, plus nearby ambient fire. No commit.

- **2026-09-08 — large-batch candidate rejected; stereo/body-transform candidate.**
  Video Project 73 (25.43s, extracted by Tools/review_flamethrower_video73.py)
  and user feedback: some fire remains head-attached; corrected component follows
  controller but has eye mismatch and is not aligned with barrel. Do NOT mark
  43137CE2 accepted. Shared muzzle private-CB path builds one first-eye result;
  immediate twins restore tracked snapshots, while replay independently builds
  M*V1. In general M*V1 != V1*inverse(V0)*M*V0 for a rotated controller, so
  that path is not a sound stereo transform for the new flame batches.

  New candidate removes large flame batches from IsMuzzleBillboardDraw and routes
  them separately. Rebuilds tracked cb0 bytes for BOTH eyes at the exact draw,
  deriving right WV as V1*inverse(V0)*leftWV. No private first-eye-only buffer and
  no muzzle replay override for these batches. Flame transform has a separate
  same-frame cache published only by the captured 40740-index weapon body after
  successful CPU/GPU substitution, rather than later combat hand parts.
  Selection remains the prior 17-128-instance exact shader/weapon heuristic;
  remaining head-attached components intentionally NOT admitted speculatively.
  Damage/aiming and accepted hand/laser changes are unchanged. Hunting stays 0.

  Release build, flame source guards and numeric stereo world-transform invariant,
  existing hand/laser checks and NPC history tests pass. No headset acceptance
  yet, and barrel alignment is not claimed solved by the math test. Archive:
  Builds/flamethrower-stereo-body-transform-20260908, with rejected DLL/runtime
  log and new source/DLL. Archive/game hashes match. Need normal firing check for
  moved component's stereo/barrel placement; full multi-layer fix remains pending.

  **Headset confirmed:** user said "Yeah that part is fixed" for the moved
  component's stereo/barrel alignment. ECCA0006 is the accepted CORE baseline,
  not a complete flamethrower fix; head-attached companion layers remain.

- **2026-09-08 — captured flame companion layers routed to accepted transform.**
  Continued existing frame history rather than asking for another capture.
  Tools/review_flame_layers.py creates amplified consecutive-RT differences.
  Draw 596 adds 4,445 substantially changed spark pixels, 602 adds 2,857 pixels
  at the flame core, 622 adds 8,283 pixels of outer smoky glow. Captured payloads
  are world-space particles around the same source. Shader/count identities:
  E1FDE/FB0FCA at 33, E1FDE/252FCE3C8790BFBC at 17, and
  240342/F5CBE24C2E4098EB at 4. These were absent from the accepted stream route.

  Candidate routes companions at 2-64 instances and outer glow at 1-16 through
  RebuildFlamethrowerStream, guarded by exact fresh flamethrower body and existing
  firing/tail state. Accepted core selection and stereo/body transform unchanged.
  Ordinary one-particle muzzle companion path and captured 104/128/200 ambient
  batches remain excluded from these additions. This remains a size/weapon
  heuristic, NOT per-particle ownership: same-size ambient batches with these
  shaders while firing this weapon still require testing. No damage/aim edits.
  Release build, flame-source/stereo invariant checks, hand/laser checks and NPC
  history tests pass. Archive Builds/flamethrower-companion-layers-20260908
  preserves accepted-core DLL and runtime log; deployed archive/game hashes
  match. Full flame visual acceptance pending, no commit.

- **2026-09-08 — residual head-attached fire; small world-space batch gap.**
  User reports some head-attached effect remains on AFB7B99E and supplies
  2026-09-08 20-47-53.mp4. Reviewed 20 frames of 32.599s video via
  Tools/review_flame_residual_video.py. Latest runtime includes 200 unhandled
  one-instance 240342/74E951 samples and 327 one-instance E1FDE/252FCE samples
  while firing with hand mesh 17784 as shared cache producer. Matching world
  transforms include BOTH identity-W world-particle batches and object-local
  effects; a one-particle count alone cannot distinguish them.

  Candidate admits core 74E951 batches below 17 and 252FCE one-instance batches,
  preserving exact weapon/shader and upper-count gates. Rebuild now checks all
  12 W components for identity in BOTH eye snapshots before writing either.
  Non-identity object-local effects fall through without camera-only rewriting.
  Ordinary FB0FCA single-particle muzzle path is unchanged. Body cache and stereo
  math remain unchanged. This fixes an observed routing gap, not proof of the
  residual visible layer's complete identity; same-size ambient world batches
  remain a heuristic risk. Do not claim full visual acceptance.
  Release build, flame-source/stereo checks, hand/laser checks and NPC tests pass.
  Archive Builds/flamethrower-small-world-batches-20260908 preserves previous
  DLL/runtime log and new DLL/source. Installed/archive hashes match. Hunting=0,
  aiming/damage unchanged. Normal firing visual check pending; no commit.

- **2026-09-08 — residual flame/smoke persists; separated current-output capture.**
  User reports flame and smoke remain HMD-attached on BF2F6E85. Current trace
  no longer lists the prior small 74E951/252FCE world-batch misses among the
  remaining firing draws. Do not infer the residual visible effect was identified
  by that routing fix. No further particle filters/transforms changed this turn.
  Installed observation-only automatic capture on current corrected output,
  requiring firing for >=30 frames and controller direction lateral fraction
  >=0.3, to separate corrected barrel plume from head plume on screen. Capture
  fires once per process. User needs fire with gun angled sideways from gaze.
  Output is `<PRIVATE_DEVELOPMENT_ROOT>/Builds/FrameAnalysis-FlameResidual-*`,
  with local deduped data (SHARE_DEDUPED off): H: has only ~3GB free and C: ~18GB.
  Earlier captures preserved. Hunting temporarily 1; MUST restore to 0 and
  disable automatic capture after collecting evidence. Normal render logic and
  damage/aim unchanged. Release build and existing hand/laser/NPC checks pass;
  clean-release flame test intentionally expects capture disabled and was not
  claimed passing for this diagnostic. Installed/archive SHA256:
  3157500D2418685E36C196D6767E397E40B35A87036BE339EEF89851A9CAF827.
  Archive Builds/flame-residual-separated-capture-20260908 preserves previous DLL,
  INI and runtime log. Residual visual fix remains pending. No commit.

- **Separated capture result:** automatic capture succeeded at frame 1459,
  Builds/FrameAnalysis-FlameResidual-20260908-205619-004. Final RT 655 shows
  corrected flame at far right; residual head-attached orange flame is not
  clearly identifiable in this one frame. Central-ROI draw differences examined
  using Tools/review_residual_capture.py: 580 adds faint blue haze (480-index
  object-local mesh), 588 adds cobwebs, 589 adds blue light/fog (two particles).
  None is established as the reported residual flame; DO NOT move these merely
  because they occupy the head-forward region. Need user confirmation/marking
  whether target is visible in final captured frame, or confirmation capture
  missed it. No rendering classifier/transform changes made. Disabled automatic
  capture and restored hunting=0; rebuilt and deployed, flame-source/stereo tests
  pass. Runtime preserved under Builds/flame-residual-analysis/capture-runtime.log.
  Correction behavior remains prior BF2F6E85 behavior; residual issue unresolved.

- **2026-09-08 — user confirms capture was too early.** The residual flame
  appears later after trigger press, smoke later still. Previous 30-frame capture
  does NOT rule out residual geometry. Observation-only capture now waits five
  wall-clock seconds of continuous input (GetTickCount unsigned elapsed time),
  restarting the delay on each new trigger press. Existing sideways-ray gate
  remains so effects are separated onscreen. Captures once per process to the
  project Builds/FrameAnalysis-FlameResidual-* with local deduped data. Rendering
  transforms/classifiers unchanged. Build and hand/laser/NPC checks pass; clean
  flame guard intentionally expects capture off and is not claimed passing for
  diagnostic. Archive Builds/flame-residual-five-second-capture-20260908 contains
  previous DLL/INI and new source/DLL; installed/archive hashes match. Hunting=1
  temporarily; MUST disable capture and restore hunting=0 after next run.
  User should fire continuously 6-8 seconds with gun angled sideways from gaze,
  then exit. No residual fix claimed, no commit.

- **Delayed capture sees residual but starts too late within frame.** User
  reports flame still HMD-attached; smoke may have followed controller, uncertain.
  Capture Builds/FrameAnalysis-FlameResidual-20260908-210421-376, frame 1914,
  clearly shows an orange head-forward flame/cloud separate from far-right
  corrected plume. BUT only 31 draws were captured, beginning at a late scene
  pass: GetTickCount crossed five seconds mid-frame, after particle producers.
  This is a capture-code defect, not inadequate user reproduction. Cannot infer
  producer from already-composited image. Preserved full capture and runtime.
  Added first-draw-only eligibility evaluation via inputSampleFrame guard;
  elapsed time/direction crossing mid-frame waits for next frame. Delay remains
  five seconds, sideways direction gate retained. Rendering logic unchanged.
  Release build and diff check pass; installed/archive hashes match under
  Builds/flame-delayed-frame-boundary-capture-20260908. Hunting was restored to
  zero during inspection then re-enabled for corrected diagnostic; MUST restore
  hunting=0 and disable capture after next successful full frame. Need repeat
  6-8 seconds sustained fire with gun sideways. No visual fix claimed.

- **2026-09-08 — full delayed capture identifies mature smoke and distant flame.**
  User confirms smoke HMD-attached. Full frame captured successfully at 2082,
  1,109 draws, Builds/FrameAnalysis-FlameResidual-20260908-210848-563. Final RT
  clearly separates barrel fire (far right) and orange cloud/burning patch at
  gaze. Tools/review_residual_capture.py now accepts capture/output arguments;
  full difference images in Builds/flame-delayed-full-analysis.
  Draw 964, VS240342/PSF5CBE, adds the entire orange cloud (105,692 significant
  changed central pixels). Identity W, 45 particles. Existing outer-smoke cap16
  explains intermittent routing as smoke grows; candidate increases this exact
  variant to128 with existing weapon/firing/identity-W/stereo guards unchanged.

  Distant visible burning patch is draws678/679/680, VS240342 / PS5BE1A3399ADF18F3,
  counts44/25/7, world origins around (30,-4,-191), distinct from near stream
  sources around z=-206 to -209. This is world burning/impact fire, NOT proven
  to be a muzzle component. Do not fold this entire shader into gun transform:
  risks moving environment/enemy burns. Its camera-related placement still needs
  investigation; mature-smoke fix does not claim to solve the remaining flame.
  Other central diff draws are cobwebs/light fog and are not candidates.
  Capture disabled, hunting restored0. Release build, flame-source/stereo tests,
  hand/laser checks and NPC tests pass. Archive Builds/flame-mature-smoke-candidate-20260908
  preserves diagnostic DLL/runtime and new source/DLL; installed/archive hashes
  match. Needs smoke visual validation; residual burning patch unresolved. No commit.

- **2026-09-08 — remaining flame candidate implemented and installed, no new capture.**
  User rejected stopping at smoke-only correction and repeated capture requests.
  Used existing full delayed frame2082: burning-patch draws678/679/680,
  VS2403427473C35A13 / PS5BE1A3399ADF18F3, counts44/25/7. Added separate
  burning-patch route, not blanket inclusion of this shared environmental shader.
  Requires current-frame dedicated flamethrower params, firing/tail gate,
  identity world matrix in both eyes, and every particle in a head-forward tube
  (depth2-30m, radius3.5m) in valid native view. Matching batches use the existing
  accepted barrel/stereo rebuild. Mixed/off-axis batches remain native.
  Captured batches pass the position test in captured eye basis (max radius
  2.245/2.332/1.596m, depths19.07-21.37m). This is a POSITIONAL HEURISTIC,
  not proven emitter ownership: environmental burns inside the tube may match;
  other distances or mixed batches may fail. Actual headset behavior unverified.

  Reads one bounded dynamic stride64 particle pool through observed CPU Map/Unmap
  writes, with first-instance/offset bounds and current-frame snapshot checks.
  No blocking GPU readback. First encounter waits for a subsequent CPU write;
  unsupported buffers fail closed. Retained pool bounded8MB; CPU snapshot copying
  has a performance cost. No damage, projectile or VRPose changes. Existing
  mature-smoke cap128 and accepted core stereo/barrel correction retained.
  Release build, flame route/stereo test, new test_flame_burn_capture.py,
  late-game hand/laser checks and NPC history executable all pass. These checks
  do not substitute for runtime visual verification. Capture remains disabled,
  hunting=0. No further capture requested. No commit.
  Archive Builds/flame-head-ray-burn-candidate-20260908 includes previous DLL,
  runtime log, source/header, capture test and installed DLL. Game was stopped.
  Installed/archive SHA256 both:
  B4DFA829737341EF369B779A0535C0A6C07C3CA1B9D764151EBFA1BBD61725AD.

- **2026-09-08 — video rejects burn candidate; older draw-state leak corrected.**
  User video `<PRIVATE_VIDEO>/2026-09-08 21-27-40.mp4` (33.182s) reports
  HMD flame remains, stream is rigid instead of sweeping, and wall fires follow
  controller. User explicitly says regressions likely predate last change:
  DO NOT attribute them solely to B4DFA829. Reviewed20 sampled video frames,
  Builds/flame-wall-regression-video, via parameterized review_flame_residual_video.py.
  Whole-batch current-pose correction cannot preserve historical emission
  motion, and head-forward proximity is NOT ownership. Removed the separate
  5BE1A339 burning-patch route, its dynamic-pool CPU copying, and its unused
  first-instance argument. test_flame_burn_capture.py now demonstrates that the
  REJECTED tube accepts world burns, and guards against restoring that route.

  Found an independent older defect in RebuildFlamethrowerStream: overwrote
  shared cb0 and BOTH tracked eye snapshots, but EndFlashParticleCB only restored
  the ordinary private muzzle CB. Flame route never set that swap flag. Metro
  can reuse cb0 across draws without Map; capture2082 draws670-682 and960-966
  use cb0 hash2b16f1d3 across shader families. Thus unclassified world effects can
  inherit the controller transform. Added draw-scoped backup/restore of both
  snapshots and actual cb0, bracketed before/after draw including immediate twin.
  Queued eye remains correct: StereoBatch::Hold COPIES bytes before AfterDraw.
  Acquisition Map now precedes snapshot mutation; failure leaves snapshots intact.
  Restore has context/buffer identity guards and retained/released buffer ref.
  Existing classifier counts, dedicated body params and accepted stereo math
  otherwise retained. No native emission/damage/aim/VRPose changes.

  This addresses state leakage and removes known unsafe burn routing, NOT a
  claim the entire flamethrower is fixed. Current-pose whole-batch transform
  still cannot preserve particle birth-pose trails. Remaining head-authored
  effect needs emitter/lifetime ownership evidence, not more shader/radius
  broadening. No new capture requested or enabled. hunting=0.
  Release x64 build passes (existing header warnings). New draw-scope source/
  lifecycle guards, flame stereo/source guards, rejected burn-capture check,
  hand/laser checks and NPC history executable pass. These are NOT runtime
  visual validation. Archive Builds/flame-draw-scope-restore-20260908 contains
  rejected DLL/runtime, new source/header/tests and DLL. Deployed with game
  stopped, archive/installed SHA256 match:
  9923550720CB37BBF98523EE4650DEBF73F385E8F70CDEE5FAA39CBA60188935.
  Branch codex/late-game-separate-hands; no commit. Headset result pending.

- **2026-09-08 — scope restore insufficient; effect-profile and local-nozzle candidate.**
  User: "Still an issue." Read current runtime and inventoried BOTH existing
  captures with Tools/inspect_flame_object_batches.py; no new capture requested.
  Found hundreds of small WORLD impact effects around (34,-1,-206), distinct
  from native stream around (29,-1,-209), sharing the admitted shader families.
  Their identity W and small counts passed the earlier classifier. Therefore
  restoring cb0 between draws alone cannot stop wall-effect reparenting.
  cb_particles b10 differs: stream templates use .1 velocity-scale or distinct
  atlas/flags; impact templates use .05 and other flags/atlas. Compared settings
  from initial frame1438 and delayed frame2082, not just one draw/count.

  Added FlameParticleProfile.h: pure C++ classifier checks shader pair plus
  12 effect-setting floats cb10[3..5], excluding camera axes/interpolation.
  Five stream profiles cover accepted core/sparks/inner/smoke. A sixth local
  nozzle profile covers frame1438 draws637/638 and frame2082 draw1070. The latter
  contributes a separate small bright flame in the head-forward region (reviewed
  001070-diff.jpg). Its W origin (28.641,-1.126,-209.649) yields native view
  (.014,-.059,1.654), and its particle positions are local. Earlier identity-W
  guard explicitly rejected it. It is not proof that every reported HMD flame
  is this small nozzle element. Local nozzle uses weapon*eyeView*W, coherent
  right-eye transform, matching W between eyes and a native-muzzle location gate.
  Stream profiles retain identity-W requirement and accepted transform.

  Read b10 via CPU Map/Unmap observation of one retained dynamic112-byte buffer,
  copying only112 bytes, requiring same context and current-frame snapshot.
  First encounter waits for a CPU write; unsupported/stale data fails closed.
  No GPU readback or new capture. Rejected profile returns before old small
  muzzle fallback, including FB0FCA single-particle while this weapon is active;
  ordinary weapons retain their route. Existing upper batch caps/firing/body
  guards, both-eye scope restoration, hands/lasers and damage remain unchanged.
  Bounded runtime profile trace (256 sampled lines) records kind/freshness.

  Compiled production header into test_flame_particle_profile.exe. Test feeds
  351 shader-matched draws from the two captures:16 stream/local-nozzle accepted,
  335 other draws excluded. This is effect-template discrimination, NOT proven
  emitter ownership across all levels. Local nozzle scale .3..1 observed across
  captures. Numeric local-W stereo composition, source/scope guards, rejected
  burn test, hand/laser guards and NPC executable pass. Release x64 build passes.
  Whole-batch current-pose stream still lacks particle birth-pose history;
  natural sweeping is NOT solved or claimed by this candidate. No headset
  result yet. No emission/projectile/damage modifications. No commit.
  Archive Builds/flame-effect-profiles-local-nozzle-20260908 preserves previous
  DLL/runtime plus new source/header/test/DLL. Deployed with game stopped and
  installed/archive SHA256 matching:
  7A568254A4CC2431CFFC827C2E1524DDB2A1CAC8B0611BEE140EAF8E1305C6EA.
  hunting=0; automatic capture remains disabled.

- **2026-09-08 — placement/wall separation ACCEPTED; stream sweeping candidate.**
  User on 7A568254: "That's fixed. Just need to fix the sweeping motion."
  This is the accepted flame/nozzle/wall-separation baseline; preserve its exact
  effect profiles, local nozzle routing and scoped stereo matrices. Archive
  Builds/flame-particle-trail-history-20260908/d3d11.accepted-placement-rollback.dll
  and accepted-placement-runtime.log preserve this accepted version.

  Added FlameTrailHistory.h for ONLY accepted world-stream profiles. Tracks
  particle continuity by effect settings + packed angular rate/atlas key and
  nearby native trajectory; this is inferred continuity, NOT native particle IDs.
  On first observation retain world affine D=inverse(eyeView)*weapon*eyeView.
  Subsequent observations retain that affine as controller moves; new particles
  receive current affine. Private VB applies inverse(D_current)*D_firstSeen to
  centres, velocity and packed interpolation delta. Existing stereo matrices
  then produce eyeView*D_firstSeen consistently for both eyes, without changing
  the accepted nozzle or wall-effect classifier. Particle dimensions/color/UV/
  animation remain untouched. This does not modify simulation/damage/projectiles.

  Matching uses shader-interpolated position (native pos + decoded delta *
  (1-cb10[1].w)), not fixed simulation endpoints. Shader's packed delta scale
  32.0004883 is used exactly; packed roundtrip tested for all65536 values.
  Duplicate same-frame draws reuse history; distinct particles cannot consume
  an entry twice in a frame. Trajectory/velocity gates, .2s expiry and2048-entry
  cap prevent unbounded storage; unmatched/recycled particles use current pose.
  Possible limitation: key/trajectory mismatches can reset a particle to current
  pose; collisions can associate the wrong particle. No native emitter hook or
  true lifetime ID is claimed. Actual visual smoothness remains unverified.

  Observes CPU writes to one retained dynamic stride64 VB (max8MB), no GPU
  readback. Validates current-frame snapshot and offset/first-instance bounds.
  First encounter/unsupported data falls back to accepted rigid stream. Copies
  source pool at Unmap; private256-record VB per stream draw. CPU copying and
  history matching add cost. These draws use immediate twin instead of queuing
  a reused private VB; same payload used in both eyes, original IA slot1 binding
  restored after draw. Local nozzle explicitly excluded from trail handling.
  Bounded pool/history telemetry records retained/born counts without captures.

  Compiled C++ history tests pass: retention vs new emission, duplicate draws,
  key collision trajectory, expiry, capacity, invalid input. Math tests check
  position/velocity/interpolation in both eyes, packed roundtrip and source
  integration. Existing effect classifier still16 accepted/335 excluded across
  351 captured draws. Scope, flame-source/stereo, burn-exclusion, hand/laser and
  NPC checks pass. Release x64 build passes. No headset sweep acceptance yet.
  Archive above includes new source/headers/tests/DLL. Deployed with game stopped;
  installed/archive SHA256 matches:
  B63381CC50EC9D07A0B2D20D05CC9EC182005936A631BE9D035E524D886E3384.
  No new capture requested; hunting=0; automatic capture off; no commit.

- **2026-09-08 — sweep video rejects centre-only trail; world sprite candidate.**
  User: "It still doesn't look right", video2026-09-08 21-59-21.mp4 (27.049s).
  Reviewed20 extracted frames in Builds/flame-trail-video-215921. Runtime shows
  1MB pool observed at1498 and active retention: at3720 retained116001/born32196,
  tracks1647. Thus trail implementation WAS running, not just warmup fallback.
  These counts do not prove correct native particle identities; matching remains
  heuristic and was not widened/tuned merely from this video.

  Found a mathematical gap: previous inverse(D_current)*D_birth compensation
  applied only to payload centre/vector data. VS constructs billboard corners
  afterward, and cb0 still applied current gun rotation to them. An old centre
  can remain in place while its sprite rotates/tilts with the gun. New candidate
  writes D_birth*position, D_birth*velocity and transformed packed interpolation
  delta directly into the private VB. Successful trail draws use plain eye0/eye1
  view matrices. Sprite facing is now native world-particle rendering, without
  live controller rotation in cb0. Current/previous particle dimensions receive
  retained uniform scale formerly supplied by the weapon matrix. Unsupported
  data aborts entire private draw preparation instead of mixing native/world
  coordinates; cb0 acquisition failure restores private IA binding immediately.

  Accepted effect/nozzle/wall profiles, local nozzle transform, history matching,
  immediate-twin strategy and restoration unchanged. First-use/missing-data
  fallback remains accepted rigid transform. No emission/damage modifications.
  New math regression distinguishes old centre-only corner result from world
  sprite result; packed roundtrip, history C++ tests, scope/stereo/source checks,
  351-capture profile classification, hand/laser and NPC checks all pass. Release
  build passes. Video does not establish this gap as sole visual cause; actual
  smoothness/appearance NOT yet headset-verified. No new capture requested.
  Archive Builds/flame-world-trail-sprites-20260908 contains previous trail DLL,
  runtime and new source/header/test/DLL. Accepted-placement rollback remains
  Builds/flame-particle-trail-history-20260908/d3d11.accepted-placement-rollback.dll.
  Deployed with game stopped; installed/archive SHA256 matches:
  5B1A8BD68647A3942F8D3FCC6D4DD506AE2265866D4B1E358E70AF3E6F9D6601.
  Automatic capture off, hunting unchanged0; no commit; visual result pending.

- **2026-09-08 — both sweep candidates rejected; accepted rendering restored with bounded identity observation.**
  User says world-sprite update still not fixed, then explicitly requests action
  after assistant stopped. Latest runtime: tracks1197, retained44200, born11892
  by1980. Existing counters cannot distinguish true births from identity failures.
  Do NOT keep widening matching thresholds without successive native records.
  Set kEnableFlameTrailRendering=false: no private trail VB binding or world-view
  replacement; accepted profile-gated rigid stream/local nozzle rendering restored.
  Added twelve-frame CPU-only identity trace at first stream observation, logging
  profile/rate/atlas/interpolated position/velocity plus missing-key, trajectory
  and already-claimed failure counters. No image, video or GPU frame capture.
  Trace and source-pool copying disable themselves after the bounded observation.
  This build is DIAGNOSTIC, NOT a sweep fix. Normal firing once after restart
  supplies missing successive-record evidence; no special capture procedure.
  Release build and profile/scope tests pass. Archive
  Builds/flame-identity-observation-20260908 preserves rejected DLL/runtime and
  new source/header/DLL. Game stopped; installed/archive SHA256 matches:
  5B02443EE6AB201A0098BD48DAA6023A5CBDBAD48AE5CC9A6C0D473A9CD22E4E.
  Placement/wall accepted baseline remains7A568254. Sweeping unresolved; no commit.

- **2026-09-08 — identity trace collected; mutable-key failure reproduced and corrected.**
  User completed diagnostic. Twelve frames1645-1656 present. Counters finish
  retained935/born356/missingKey219/trajectory67/claimed70. Successive native
  trajectory comparison (Tools/analyze_flame_identity.py) shows angular-rate
  changes on living particles at simulation updates: e.g.1646->1647 has19
  close trajectories with changed rates;1654->1655 has55. Unchanged simulation
  intervals preserve most rates. Therefore angular rate is not immutable ID;
  hashing it broke continuity repeatedly. Do not reinstate this hard key.

  Production now keys history by verified effect template only and uses the
  existing one-to-one position/velocity trajectory gate to match within it.
  No radius/velocity/expiry thresholds widened. Replay of ALL logged records
  through actual C++ FlameTrailHistory (Tools/replay_flame_identity.cpp) compares
  previous rate-key vs template-key: births356->180, retained935->1111. Frame
  1647 births90->71,1655 births353->179. This improves observed continuity;
  does not prove every correspondence or every visual symptom solved.
  Native interpolation advances~17ms on1651->1652 while GetTickCount64 measured
  31ms. Replaced quantized clock with cached-per-frame QPC time for prediction.

  Re-enabled trail rendering with world-space sprite approach. Detailed identity
  trace off in this mode; bounded usual trail counters remain. Accepted flame
  profiles/nozzle/wall exclusions unchanged. No damage/emission modifications.
  Replay/source guards, world-sprite/packed/stereo math, history, scope,351-draw
  profile test, hand/laser/NPC checks and Release build pass. Still needs headset
  visual acceptance. No new capture requested. No commit.
  Archive Builds/flame-stable-template-trail-20260908 preserves diagnostic DLL,
  identity-runtime.log (permanent replay fixture), new source/header/tests/DLL.
  Game stopped; installed/archive SHA256 matches:
  84B3E251469FBD453ECA3A2AD6E735D2AD5005B4FA64A535F0BDD6A3FCB4C619.

- **2026-09-08 — sweeping ACCEPTED; final cleanup and integration.**
  User: "Yeah that fixed it", then requests all notes, removal of diagnostics,
  embedding, commit and merge if needed. 84B3E251 is the headset-confirmed full
  flame baseline, including sweeping; earlier placement/nozzle/wall separation
  also confirmed. Verified behavior is already embedded in C++ DLL sources:
  late-game separated hands/sleeve hiding/watch timer, player-vs-NPC laser
  ownership/head-dot/range-depth corrections, flame effect profiles/local nozzle,
  stable-template trajectory history, QPC time, world sprites and stereo restores.
  No new loose shader/calibration/configuration dependency requires embedding.

  Removed this task's flame route/bound-object/profile/pool/identity/history
  logging, laser load/matrix/light/depth logging, diagnostic-only native-object
  snapshots and mesh/flag/NPC-decision trace state, plus the flash-gate trace
  that had been re-enabled. Restored pre-task disabled automatic muzzle capture (discarded temporary
  delayed/sideways/capture-directory modifications). History statistics compile
  only under FLAME_TRAIL_TEST_METRICS in standalone replay test, not DLL. Kept
  functional native CPU snapshots and history required by the verified fixes.
  No source-added runtime LogInfo calls remain in this task's diff. Retained
  archived evidence/videos/rollback DLLs; "remove diagnostics" does not delete
  the only regression evidence or unrelated project work.

  Final Release x64 build passes. C++ trail/NPC tests, recorded identity replay
  (356->180 births unchanged),351-draw profile classification, hand/laser guards,
  world-sprite/stereo/packed math and draw-scope tests pass. New cleanup test
  verifies diagnostic markers absent from source AND compiled DLL. Diff check
  passes. Cleaned binary has no separate headset run; behavior is preserved from
  accepted build rather than claimed newly headset-verified.
  Installed with game stopped; archive/installed SHA256:
  1A12BEB38F6FEB8872A9232C638E5A759DA5ED8C0FAA3766036D0208F3B880FE.
  Archive Builds/late-game-hands-lasers-flame-final-20260908 contains clean DLL,
  accepted84B3 rollback/runtime and README. hunting=0, automatic capture disabled.
  Source, focused tests and release notes selected for commit; large captures,
  exploratory files and unrelated town-verifier line-ending status excluded.
  master remains branch starting point7cc6ce2, allowing local fast-forward
  integration after commit. No remote push requested.

- **2026-09-08 — late-game weaponless left-hand pose candidate.** User screenshot
  `Screenshot 2026-09-08 223508.png` shows the left hand rotated incorrectly
  while both physical controllers are held straight; the physical watch follows
  the same bad wrist basis. The current runtime ends with the exact 17,784-index
  late-visible hand identity, so this is not a new mesh and not the earlier
  missing-watch-parent failure. The existing route deliberately retained this
  skeleton's live Metro palette. In the town-like weaponless state Metro changes
  that idle pose, twisting both the routed left hand and its correctly parented
  watch.

  Embedded the 17 directly weighted left-hand rows from the existing captured,
  headset-accepted 17,784 palette as exact IEEE-754 words. Only the exact
  late-visible skeleton substitutes them; the right-hand rows, normal gameplay,
  post-clothing/prologue families, scripted whole-viewmodel path, watch child
  transforms, camera/input, lasers, flames and projectiles are unchanged. Also
  corrected the active late-left list to match captured topology: rows48/51 are
  not weighted by this disconnected hand component. The focused verifier now
  proves the embedded words equal the captured palette and cover every actually
  weighted left-hand row.

  Release x64 builds successfully with existing third-party warnings. Focused
  hand, cleanup, laser, flame, draw-scope and native-camera regression checks
  pass. No diagnostic logging or loose calibration dependency added. Archived
  under `Builds/late-game-town-left-hand-pose-20260908`, preserving the exact
  installed baseline (`1A12BEB...`) and pre-change runtime. Metro was closed;
  candidate and installed SHA256 match:
  `3F37EE0232C5B80D01F535A7BC959E696AF9BC5AD5B5FD31D79369B93FCE9A3C`.
  Headset validation in the reported area is pending; no commit.

- **2026-09-08 — late-game weaponless pose candidate rejected; exact placement
  trace prepared.** User reports no visible change and clarifies that only the
  hand/watch position and rotation need to match the rest of the game. Removed
  the embedded late-visible palette substitution completely. The new run log
  shows both the reused 21,252 prologue family during the load and a later
  non-prologue hand route, so the earlier assumption that the screenshot alone
  identified the visible mesh was insufficient.

  Added a bounded, behavior-neutral 24-sample trace at the final hand placement
  solve. It records the selected skeleton kind, saved normal/prologue calibration,
  evaluated wrist, and exact final 3x4 hand/watch matrix. It changes no rendering,
  input, camera, pose, or game state and needs only one ordinary affected-save
  load. The prior accepted clean DLL remains archived as rollback. Release x64
  and focused hand/cleanup checks pass. Deployed with Metro closed; archive
  `Builds/late-game-hand-placement-trace-20260908`; installed SHA256:
  `CDA594D8699B2CC05A9661B57E5E74FCC4F74D39AEAB0AD26CA9183D7DA03775`.
  No commit.

- **2026-09-08 — late-game hand/watch authored-basis correction deployed.** The
  requested affected-save trace records 24 consecutive visible samples as the
  exact `late-visible` route. Every sample uses the normal saved offset
  `(0.004, 0.172, -0.178)` and rotation `(65.89, -25.21, 37.24 degrees)`, and
  the evaluated wrist remains stable, ruling out wrong routing, calibration
  selection, and wrist animation as the cause. Offline extraction of the normal
  12,132-index reference from `watch_capture.rdc` plus the existing late-visible
  capture confirms the relevant distinction: the fixed late hand palette has
  an identity root while normal gameplay is authored under its captured
  controller-local basis.

  The placement solver now selects an identity authored basis only for the
  prologue and exact late-visible mesh. Normal gameplay retains its proven
  captured basis, and the late mesh can neither capture nor poison that normal
  cache. Translation is recomputed from the anatomical wrist after the corrected
  rotation, while the watch receives the identical corrected rigid orientation
  and its stable wrist reference. Removed the bounded placement trace. Focused
  topology/basis and release-cleanup tests pass; Release x64 builds successfully.
  Metro was closed for deployment. Candidate/installed SHA-256:
  `49418826368EF3E9EA292257DB1DF25E2A4A25F2D6D3628CA76040432D33C29D`.
  Archive: `Builds/late-game-hand-watch-basis-fix-20260908`, including the
  diagnostic rollback and trace evidence. Headset validation pending; no commit.

- **2026-09-08 — basis-only correction rejected; normal hand pose mapped onto
  late-visible skeleton.** User reports the hand and watch remain wrong and asks
  whether using the same pose as the other hands would simplify the fix. It
  does: the earlier palette candidate copied the captured late palette, whose
  relevant root is effectively identity, so it preserved rather than replaced
  the reported pose. The later identity-basis-only candidate also did not match
  the headset reference and is superseded.

  Extracted the normal 12,132-index hand from the existing watch capture and
  compared it vertex-by-vertex with the isolated 17,784-index late-visible
  hand. Per-vertex weights establish a direct 17-bone correspondence: normal
  pose entries 0..14 map to late bases 3..45, and normal entries 16/17 map to
  late bases 54/57; unused intervening rows are deliberately skipped. Applying
  those normal matrices to the late geometry reproduces the normal captured
  hand at approximately 2.6 mm RMS despite minor mesh variation.

  The exact late-visible route now loads the same embedded/external preferred
  pose as normal gameplay and copies it through that mapping. Its final position
  remains owned by the existing anatomical wrist constraint and normal saved
  offset/rotation; the watch again shares the normal authored-basis orientation
  and rigid wrist parent. Normal, prologue, full-body late, and right-hand paths
  remain unchanged. Focused topology/mapping and release-cleanup tests pass;
  Release x64 builds successfully. Candidate SHA-256:
  `499B4C37E8B4DF8678FE078A0EDB73F682291BCDC4F9E7F8155720D6F2D52D7D`.
  Archive: `Builds/late-game-normal-hand-pose-map-20260908`, with basis-only
  rollback. Headset validation pending; no commit.

- **2026-09-08 — existing controller recalibration extended to left-hand render
  basis.** User requests that the same held right-controller recalibration which
  repairs a bad startup gun/right hand also repair the left hand, particularly
  when that controller was outside reliable tracking during startup. The action
  already cleared the left controller rotation/position anchors in `VRPose`, but
  the render-side hand solver retained its separate static controller-local
  authored basis. That stale second cache could therefore survive recalibration.

  Added a thread-safe monotonic controller-recenter generation advanced by both
  explicit aiming recalibration and a SteamVR tracking-origin recenter. The hand
  placement solver observes it and invalidates its cached gameplay basis, so the
  next valid tracked left-controller samples reacquire both left anchors and the
  rendering basis together. This preserves the user's saved hand offsets,
  rotations, normal-pose mapping and watch calibration; no new gesture or loose
  setting was introduced. Focused hand/cleanup tests and native state, camera
  lineage, instance and look-gate suites pass. Release x64 builds successfully
  with one existing third-party warning. Candidate SHA-256:
  `235D02F12ABFD593B711A04BC79C4FDA87C53970F6A279C655DFFD2864EACAA1`.
  Archive: `Builds/left-hand-shared-recalibration-20260908`, including the prior
  normal-pose candidate as rollback. Headset validation pending; no commit.

- **2026-09-08 — shared left-hand recalibration headset-accepted.** User confirms
  the existing right-controller recalibration now repairs the left hand as
  intended. Accepted SHA-256:
  `235D02F12ABFD593B711A04BC79C4FDA87C53970F6A279C655DFFD2864EACAA1`.
  Preserved as the rollback for the next prologue scripted-arms candidate.

- **2026-09-08 — prologue complete scripted arm/hand priority restored.** User
  reports that prologue scripted scenes no longer show the normal authored arms
  and hands, although this worked previously. Source history isolates the
  regression: a later Defense experiment moved normal-town/post-clothing hand
  authority ahead of the positive prologue scripted route. That early return
  could discard the only complete 21,252-index animated arm-and-hand mesh before
  its native two-eye draw.

  Restored the previously working ordering only inside the exact opening mesh
  branch: positively owned scripted performances render the complete native
  mesh in both eyes before ordinary town suppression is considered. Outside a
  scripted performance, existing town and clothing hand authority is unchanged.
  A post-clothing hand-only draw is also suppressed while the complete scripted
  arms are current, avoiding duplicate hands. A new focused source regression
  test verifies ordering, both-eye submission and duplicate suppression. It,
  the late-game hand/recalibration guards, release cleanup, and native state,
  camera-lineage, instance and look-gate suites pass. Release x64 builds with
  zero errors. Candidate SHA-256:
  `AD288D2EE057CC27BA13BC55A6976F6A2FDD0239D3E9A5F23A2FC3020EFB31DE`.
  Archive: `Builds/prologue-scripted-complete-arms-20260908`, with accepted
  recalibration rollback artifact. Headset validation pending; no commit.

- **2026-09-08 — prologue priority candidate rejected; ownership trace
  prepared.** User reports no improvement. The new run log makes the failure
  concrete: the exact 21,252-index prologue mesh is present and wrist-locks to
  the left controller at observed frames 715 and 1,637, which means it remained
  on the ordinary split-hand route. Positive `ScriptedHandPerformanceActive`
  ownership never asserted, so reordering a branch that was never entered could
  not restore the arms.

  Removed the ineffective priority and duplicate-suppression changes. Added a
  bounded read-only trace on the exact prologue mesh, sampled every 15 frames
  for at most 160 samples. It records the raw player owner/sentinel/mode/
  performance/normal-control fields, native look permission, exact native
  viewmodel and camera-effector flags, derived player/native/scripted hand
  ownership and the non-authoritative loose camera classifier. It performs no
  GPU readback and changes no draw, pose, camera or input behavior. Release x64
  builds with zero errors; focused hand and cleanup tests pass. Diagnostic
  SHA-256:
  `F297570CB8C82AE7D9F1392A668A9707FFFB8890408EA6C703D9964D7524F16B`.
  Archive: `Builds/prologue-scripted-ownership-trace-20260908`, containing the
  rejected run and accepted recalibration rollback. One affected prologue scene
  run is required; no commit.

- **2026-09-08 — prologue ownership confirmed; replacement-geometry census
  prepared.** The requested ownership-trace run disproves the prior diagnosis.
  The exact 21,252-index mesh is observed with all scripted flags clear through
  frame 1,680. At frame 1,695, the raw performance and look flags, exact native
  viewmodel/camera signals, and every derived scripted-hand ownership signal are
  positive. They remain positive when the mesh returns at frames 3,360–3,570.
  The absence of any exact-mesh samples between frames 1,695 and 3,360 means that
  the known complete mesh itself is not submitted through the affected scripted
  interval; changing its branch priority could not affect what the user sees.

  Removed the completed ownership-state trace and replaced it with a bounded,
  behavior-neutral geometry census. Once the known prologue mesh arms the probe,
  every unique instanced draw using either the viewmodel pass or a matrix input
  is logged while positive scripted-hand performance ownership is active. The
  record includes draw topology, offsets, shader hashes, pass classification,
  matrix classification, pending vertex-buffer identity and frame number. It is
  capped at 192 signatures, performs no GPU readback, and changes no rendering,
  pose, camera or input behavior. Release x64 builds successfully with the one
  existing warning. Diagnostic SHA-256:
  `8A0168BF8CE331F7FFA7FB105198D6A1AD638351A2710CC95BBDC97B44602DA6`.
  Archive: `Builds/prologue-scripted-geometry-census-20260908`, with the accepted
  recalibration rollback and the decisive ownership log. One run through the
  same affected interval is required; no commit.

- **2026-09-09 — first prologue geometry census narrowed; missing submission
  path added.** The requested run produced all 192 permitted signatures in
  frames 1,250–1,251 because the initial matrix-layout condition admitted the
  level's ordinary instanced world geometry. Six entries were positively in the
  viewmodel pass, and all are already-known components: the 21,252-index complete
  prologue arms, 6,108/2,628/750 watch passes, and the unrelated six-index
  197-instance utility draw. No replacement instanced arm family exists in the
  captured set.

  The first probe also covered only `DrawIndexedInstanced`; repository history
  confirms the earlier Defense investigation needed a separate non-instanced
  `DrawIndexed` census for complete scripted geometry coverage. Revised the
  probe accordingly: both paths now require the positive viewmodel pass, use
  independent 192-signature caps, and the non-instanced record includes its
  exact index range and shader pair. Generic matrix-layout world draws are no
  longer admitted. This remains text-only, performs no GPU readback, and changes
  no draw, pose, camera or input behavior. Focused hand/cleanup tests and diff
  check pass; Release x64 builds with zero warnings/errors. Diagnostic SHA-256:
  `605E1D7E29253E249EF1D329C9BF262E1FCCFEC975BA48395809A3FDE20C68D3`.
  Archive: `Builds/prologue-scripted-complete-census-20260909`, preserving the
  first-census DLL/log and accepted recalibration rollback. One repeat of the
  same affected interval is required; no commit.

- **2026-09-09 — complete prologue census identifies early ownership gap;
  scoped fix prepared.** The second requested run closes the geometry question.
  All 192 non-instanced signatures are six-index UI glyph quads using one shader
  pair. The instanced path contains only the known 21,252 arm mesh and watch
  assembly before the gap, a six-index utility/particle batch during it, and
  unrelated weapon components after it. There is no hidden replacement arm
  family to route.

  The same run provides the missing lifetime evidence. The existing sustained
  scripted-camera detector engages at frame 1,351, while the validated player/
  native hand-performance owner does not engage until roughly frame 1,531. The
  exact prologue arm draw at frame 1,422 therefore still takes the ordinary
  controller split, which deliberately omits both sleeves. This explains why
  restoring priority inside the later player-owned window made no visible
  difference.

  Removed both census paths and their arming state. Added a prologue-only arm
  ownership accessor combining the existing positive hand owner with the
  already-established sustained camera-motion signal. Only the exact 21,252
  opening/prologue mesh consumes it, and it is evaluated before normal-town or
  post-clothing gameplay suppression. Generic viewmodel, gun, late-game hand,
  camera, input and culling routing do not consume the motion signal. Added a
  focused source regression test for ordering, signal scope and diagnostic
  cleanup. Focused prologue, late-game hand and release-cleanup tests pass;
  Release x64 builds successfully with one existing third-party warning.
  Candidate SHA-256:
  `653F4485DF2724FEE06A99C9077FA5BA596344A254C5972C96B5DA0AC76D18E0`.
  Archive: `Builds/prologue-scripted-early-motion-owner-20260909`, containing
  the complete census evidence and accepted recalibration rollback. Headset
  validation pending; no commit.

- **2026-09-09 — early-motion prologue candidate rejected; authored instance
  route prepared.** User reports no arms and clarifies that both hands remain
  visible: the left is the mod's forced controller hand while the right retains
  Metro's animation. This directly identifies the ordinary 21,252 split path,
  which draws both hand islands independently and omits both arm/sleeve ranges.

  The rejected run records that split at frame 1,424 and does not engage the
  sustained camera detector until frame 1,431, seven frames after the relevant
  arm submission. Timing explains why the motion-owner candidate was inert.
  Across three independent runs, the affected scripted arm draws consistently
  bind their 64-byte instance at slot-1 offset 0, while normal controller-based
  prologue gameplay after the scene consistently uses offset 64. This structural
  draw distinction exists before either software ownership detector engages.

  The exact 21,252 prologue branch now preserves the complete native arms/hands
  when that authored offset-0 instance is bound, or when either established
  scripted owner is already positive. Offset-64 ordinary gameplay retains the
  established separated controller hands, wrist/watch parenting, and sleeve
  omission. No other mesh family, gun, camera, input, culling, or recalibration
  route uses the new condition. Updated the focused regression test. Focused
  prologue, late-game hand and release-cleanup tests pass; Release x64 builds
  with zero warnings/errors. Candidate SHA-256:
  `B76E4923F46CB76980794667C6A6A2DA25F1D95A5925DBAACB6075CAD35FADD1`.
  Archive: `Builds/prologue-scripted-authored-instance-20260909`, including the
  complete census, rejected-run evidence and accepted recalibration rollback.
  Deployed for headset validation; no commit.

- **2026-09-09 — authored-instance route confirmed but visually rejected;
  complete-arm scissor fix prepared.** User reports no visual change. The new
  headset log contains the recognized opening-family watch submissions but no
  periodic `left controller attachment` entry, unlike every run through the
  ordinary 21,252 split. This confirms the complete native route replaced the
  forced-left split; the remaining failure is downstream visibility rather than
  ownership or mesh selection.

  That early complete-mesh return bypassed the no-scissor protection already
  proven necessary for other authored scripted hand families. The prologue's
  full 21,252-index draw now receives the same raster-only protection in both
  eyes, with the exact incoming raster state restored afterward. A one-shot
  route record reports the original scissor state and whether the replacement
  state was available, making this headset run conclusive. Focused prologue,
  late-game hand and release-cleanup tests pass; Release x64 builds with zero
  warnings/errors. Candidate SHA-256:
  `EE2D006FBD3E1738BBA86DDAC891FF9EF8A4D0C49522C8C1F7F72BB21E63E257`.
  Archive: `Builds/prologue-scripted-complete-no-scissor-20260909`, preserving
  the rejected run and both rollback levels. Installed DLL/PDB match the
  archived candidate byte-for-byte. Headset validation pending; no commit.

- **2026-09-09 — complete-arm and no-scissor paths confirmed inert; known-good
  scripted-hands A/B deployed.** User reports no visual change. The decisive
  route line proves the complete 21,252 draw ran at frame 715 with offset 0,
  Metro's source scissor already disabled, and the replacement no-scissor state
  available. The same run does not submit that arm mesh continuously through
  the later scripted interval. Ownership, branch order, full-index submission,
  and raster clipping are therefore ruled out as explanations for the visible
  controller-left/animated-right pair.

  Preserved the rejected run as
  `Builds/prologue-scripted-complete-no-scissor-20260909/rejected-no-scissor-run.log`.
  For a controlled historical comparison, deployed the archived build that the
  user previously confirmed kept hands attached to arms during scripted events:
  `Builds/scripted-hands-ignore-stale-normal-control-20260905/d3d11.dll`, SHA-256
  `0DC795998ECD5EA9F79222F18B97CD0822494488C5BA64A87653A301D1552460`.
  This is an A/B diagnostic rather than the final integration build; the current
  source candidate and both rollback binaries remain preserved. No commit.

- **2026-09-09 — known-good A/B succeeds; shared 17,784 classifier regression
  isolated and fixed.** User confirms the archived `0DC795...` build shows the
  arms correctly in the exact affected prologue. Its new log also proves that
  build still split the 21,252 opening mesh at frames 715 and 1,600. Therefore
  the opening mesh, its ownership, and its raster state were never the source of
  the visible arms.

  The actual behavioral difference is the later 17,784-index late-game visible
  arm classifier. The prologue submits that same exact mesh/shader identity a
  few frames after its recognized opening family. Newer code classified it as
  late-game geometry globally, omitted both sleeve ranges, and routed the left
  hand independently—the user's precise forced-left/animated-right symptom.
  The working old build predated that classifier and rendered 17,784 whole.

  Restored the old whole-viewmodel behavior only when the 17,784 identity occurs
  within 16 frames of the recognized opening family. All other 17,784 draws
  retain the headset-accepted late-game split, pose mapping, watch parenting and
  shared recalibration. Removed the three rejected 21,252 ownership/offset/
  no-scissor experiments. Focused prologue, late-game hand and release-cleanup
  tests pass; Release x64 builds with zero errors and one existing third-party
  warning. Candidate SHA-256:
  `0CAC6C303260F6FEBE5AC4AFF90822BA5522245D4D5D59B95BA71894F158470D`.
  Archive: `Builds/prologue-17784-complete-handoff-20260909`, preserving the
  user-confirmed old binary/log and accepted recalibration rollback. Headset
  validation pending. Installed DLL/PDB match the archived candidate
  byte-for-byte; no commit.

  **Headset accepted:** user confirms the prologue arms are restored. The
  successful log records the new route at frame 1,589 with `openingAge=0`,
  proving the exception is tied to a same-frame opening-family handoff. In two
  captured affected late-game runs, the opening family appears at frame 715
  and the 17,784 late-game mesh at frame 1,127 or 1,149—a 412/434-frame gap,
  safely outside the 16-frame window. The change does not alter watch parenting,
  recalibration generation, hand pose data, lasers, flames, guns, camera, input,
  or any mesh other than this exact contextual 17,784 classification. Successful
  runtime log archived as `headset-accepted-runtime.log`; no commit.

- **2026-09-09 — intermittent early prologue forced look bridged.** After the
  arm restoration was headset accepted, the user reported that forced look did
  not always hide the hands below the screen during the prologue. The accepted
  runtime log explains the gap: sustained scripted-camera motion engaged at
  frame 1,331, but rendered pitch remained gated until exact native camera
  ownership arrived shortly before frame 1,587. `GetScriptedPitchOffset()`
  intentionally ignored the broader motion detector to avoid gameplay false
  positives.

  Added a prologue-only handoff: the offset-0, stride-64 opening viewmodel may
  arm the already-sustained motion signal for at most 180 frames, and exact
  native ownership takes precedence as soon as it appears. Loading clears the
  marker. This does not broaden ordinary gameplay detection; in captured
  late-game runs the shared opening mesh occurred before sustained motion and
  therefore cannot arm the bridge. A one-shot log records
  `prologue forced-look bridge armed` when the route is used.

  Focused prologue forced-look, scripted-arm, late-game hand-split, and release-
  cleanup tests pass. Release x64 builds with zero errors and one existing
  third-party warning. Candidate SHA-256:
  `ED42F9CF93F065A66EA86F6E9E4828EEF688C9DAA5DE3E585723222502D38DEF`.
  Archive: `Builds/prologue-forced-look-bridge-20260909`, including the prior
  headset-accepted DLL/PDB and runtime log as rollback evidence. Headset
  validation pending; no commit.

  **Headset rejected and reverted:** user reports no improvement and chose not
  to spend more time on this intermittent issue. Removed the prologue motion
  bridge and its focused test, then restored the immediately preceding
  headset-approved prologue-arms DLL/PDB (`0CAC6C...`). The accepted arm,
  hand/watch, and shared recalibration changes remain intact. The rejected
  candidate stays archived for evidence only; no commit.

- **2026-09-09 — Market missing forced-hands candidate.** The current Market
  runtime reaches gameplay, loads both controller-hand calibrations, and
  anchors the left controller hand, but Metro can retain its broad player-
  performance hand flag during ordinary controllable town play. The generic
  12,132/post-clothing hand route previously treated that broad flag alone as
  authored ownership and could return the hands to Metro's normally off-screen
  transform after controller routing had initialized.

  Generic town hands now preserve the complete authored viewmodel only when
  the broad performance flag coincides with the already validated native-look
  camera gate. Exact native viewmodel effectors remain authoritative. The
  opening/prologue arm family keeps its separate earlier ownership path, and
  the late-game 17,784 exception is unchanged. A focused source test guards
  this boundary. The Market, prologue-arm, late-game hand, release-cleanup, and
  town camera ownership checks pass. Release x64 builds with zero errors and
  one existing third-party warning. Candidate and installed SHA-256:
  `CD78FFA8CCF93B32DD51306068BDA7DD9DE5FA1C6A7F4D529D3861243D0F245C`.
  Archive: `Builds/market-forced-hands-native-look-gate-20260909`, including
  the pre-change Market log and exact headset-approved `0CAC6C...` rollback.
  Headset validation in Market pending; no commit.

  **Headset accepted:** user confirms the forced controller hands are restored
  in Market. The candidate remains installed while the next town-watch issue is
  addressed; no commit.

- **2026-09-09 — Market/Armory watch-time fallback candidate.** User reports
  that the controller-parented physical watch has no time display in Market or
  Armory. The Market run confirms the left-hand wrist landmark and watch parent
  initialize, but contains no successful watch-glyph decode or validated timer
  texture. These safe areas do not reliably emit the HUD HH:MM batch that
  feeds the custom 3D display.

  `DrawWatchDisplay()` now continues to prefer a fresh decoded Metro value for
  filter countdowns and authored timer states. When no complete decode exists
  or it is older than 180 frames, it falls back to local system `HH:MM`, matching
  Metro's real-time watch source. The fallback still requires the existing
  current physical-watch casing parent, so it cannot appear as detached HUD
  text. The town watch fallback, Market hands, prologue arms, late-game hands,
  release cleanup, and town camera ownership checks pass. Release x64 builds
  with zero warnings and zero errors. Candidate and installed SHA-256:
  `06A322322E702F4E1437695AE0F00E591DCA78CDF5806A144B35A79255A7B2E3`.
  Archive: `Builds/market-armory-watch-time-fallback-20260909`, including the
  accepted Market-hands rollback and its missing-time runtime log. Headset
  validation in Market and Armory pending; no commit.

  **Headset accepted:** user confirms the time display is restored. Release
  closeout removed the temporary one-shot prologue 17,784 handoff route log and
  added a regression assertion that it remains absent. The accepted late-game
  hand pose mapping uses the already embedded normal-game pose, shared
  recalibration is represented entirely by the in-DLL generation state, the
  prologue mesh exception is deterministic, and the Market/Armory clock
  fallback reads local system time directly. No new learned or calibration file
  is required by any completed fix in this branch.

- **2026-09-09 — hand-fixes release closeout.** Rebuilt after diagnostic
  cleanup with zero warnings and zero errors. Focused Market-hand, town-watch,
  prologue-arm, late-game hand, release-cleanup, laser-route, camera-lineage,
  early-camera, town-ownership, and native-only release guards pass. The
  signature verifier that requires a supplied executable image was not run as
  a standalone no-argument test; its surrounding native-only delivery guard
  passed. Final installed DLL/PDB match the archived release byte-for-byte.
  DLL SHA-256:
  `71C6FB69446BA317225F21FFA2113E4F533F8181C33078466B6B2AE250BEB4FB`.
  Archive: `Builds/hand-fixes-release-20260909`. No completed fix requires an
  external learned file. Ready to commit and fast-forward into `master`.

- **2026-09-09 — VR menu revamp candidate deployed.** Replaced the prior five-
  tab overlay with Picture, Controls, UI, and Advanced pages. AER selection is
  removed and older saved AER values migrate to True Stereo. Body typography is
  smaller, row spacing is uniform within each page, and the selection outline
  is generated from the same registered row bounds used by navigation, removing
  the old independent highlight-coordinate table.

  Picture now presents current VR resolution, stereo separation and resolution/
  world/brightness sliders, five resolution presets (Low through Max with their
  calculated dimensions), Enhanced Blacks, Expanded Visibility, and a category
  reset. Controls, UI, and Advanced contain the requested complete option layout.
  Existing settings remain connected. Separate Hands, scope controls, Ammo
  Counter, and Advanced offset editing are visibly marked not connected until
  their complete subsystem behavior is implemented and headset-tested; the
  established LT + left-grip weapon calibration remains authoritative.

  Opening the menu at a below-1.0 resolution scale now keeps compositor output
  at the native per-eye resolution instead of downsampling the overlay with the
  game. Closing the menu restores the configured submission scale. Expanded
  Visibility is saved/default-on and gates only the v26 bounded main-world
  visibility envelope, exactly as specified in Note 34. OFF passes Metro's
  original frustum through; it does not disable culling or alter native camera/
  body turning, input switching, camera credits, or Defense fixes.

  Release x64 builds with zero errors and one existing third-party C4250 warning.
  The 2,893 coverage checks, 21 mocked adapter checks (including toggle-off pass-
  through), native-only guards, late-game cleanup, prologue arms, and focused VR
  menu verification pass. Initial candidate DLL SHA-256:
  `FDC7D927E7A4725F124DDA24CC924E5CBDE1AFABB053422AF92760E8987DAA1C`.
  Archive: `Builds/vr-menu-revamp-20260909`. Headset validation pending; no
  commit.

  Follow-up correction: Scope Sensitivity is a slider, not a checkbox. The
  inactive presentation now shows a 0.05–0.30 m distance range and the current
  0.12 m engagement threshold; runtime wiring remains deferred to its focused
  functionality pass. Rebuilt and installed final candidate DLL SHA-256:
  `9CF83ED33F6D8D3372104B00CB1191A8A7BD4EF161D17CFCE12328023F5DF642`.
### Gamepad-mode native timer startup suppression — 2026-09-09

- Gamepad Mode could expose Metro's native flat time/timer on the HMD. Turning
  Gamepad Mode back off and restarting did not hide it because the menu setting
  was already correctly saved as `gamepad_mode=0`; the renderer's suppression
  set instead started empty on every process and waited for a watch-icon batch
  that this HUD path does not necessarily submit.
- Seeded the suppression set with the repeatedly headset-validated Spartan
  timer atlas (`35e3c17c`, `512x512`). The existing narrow classifier still
  requires the generic text shader, a six-index glyph, the exact atlas, and a
  five-glyph same-frame cap. The custom physical wrist display continues to
  decode the same source draws, and dynamic learning remains available for
  Survival or future atlas variants.
- Release x64 built with 0 warnings and 0 errors. The watch fallback guard,
  2,893 coverage checks, 21 adapter checks, menu verification, native-camera
  ownership, late-game cleanup, and prologue scripted-arm guards all pass.
  Source, archive, and installed DLL SHA-256 match:
  `90435FA83F9EE8883843CA111FA8F7A746AF62C162C666DC370CC57DA0226754`.
  Headset validation is pending.

### Embedded final weapon calibrations — 2026-09-09

- Imported all 11 exact v4 records from the installed
  `vr_weapon_calibrations.txt` into the DLL as compiled defaults. This includes
  every calibrated position, rotation, stable weapon key, body mesh, and full
  compatibility mesh set.
- The machine-local file is still read after the embedded table and compacted
  by weapon key, so it remains a last-wins override for future recalibration.
  With the file absent, the same profiles and registered body identities are
  available directly from the DLL.
- A focused regression guard verifies the 11 keys, each mesh count, the exact
  normalized profile-table SHA-256 (`2D5E801A...`), and embedded-before-file
  load ordering. Release x64 built with 0 warnings and 0 errors; all menu,
  visibility, watch, camera-ownership, late-game cleanup, and prologue-arm
  guards remain green. Candidate/source/archive/installed DLL SHA-256:
  `8E71D1F62602E2D540397C243CA519C5D6A9ACB7BC56D95F464A0D8D0AFF60F3`.

### Persisted Gamepad Mode menu visibility — 2026-09-09

- Reproduced from the user's saved state: `gamepad_mode=1`, the L3+R3 chord
  opened the menu and captured input, but the overlay remained invisible after
  restarting Metro. The menu renderer was independently gated by
  `IsGameplayModeActive()`.
- On a cold start in Gamepad Mode, physical XInput passes through and can bypass
  the synthetic startup-confirm transition that establishes the gameplay-HUD
  latch. The menu state therefore opened correctly while its renderer returned
  before initialization/draw.
- Removed only that renderer gate. An explicitly open VR menu now draws at the
  frame boundary regardless of the gameplay classifier; the fix does not force
  gameplay active or alter camera, culling, input ownership, Gamepad Mode, or
  native-timer suppression. A focused guard rejects reintroducing the coupling.
- Release x64 built with 0 warnings and 0 errors. The menu, embedded-weapon,
  watch, visibility, camera-ownership, late-game cleanup, and prologue-arm
  guards all pass. Source, archive, and installed DLL SHA-256 match:
  `EF81C4AEB1A063ED2DB90A01626A08C61D3780CA16C7BE74ABEEDF119C73B2EB`.
  Restart-in-Gamepad-Mode headset validation is pending.

### Ammo Counter menu toggle — 2026-09-09

- Connected the UI-page Ammo Counter checkbox. It defaults enabled, persists as
  `ammo_counter_enabled`, and participates in UI-category reset.
- OFF suppresses the exact numeric ammo draw identified by Metro's unique
  magazine-icon anchor. The anchor continues running so the gameplay-HUD latch
  remains intact. The custom weapon-mounted counter path, if re-enabled later,
  is gated by the same setting.
- Deliberately did not broadly suppress the surrounding panel and auxiliary
  icon shaders: project history established those shaders are shared with the
  pause menu, and hiding them by identity previously erased unrelated UI. This
  is the past fix most at risk from broadening the toggle.
- Release x64 built with 0 errors and the existing third-party C4250 warning.
  Menu, embedded-weapon, watch, visibility, camera-ownership, late-game cleanup,
  and prologue-arm guards all pass. Candidate/source/archive/installed DLL
  SHA-256: `47F88D73A5A1ACF7193BDDDE7EF56C037E249ECAB3D5DFDBDB29C8708CD10699`.

### Ammo Counter orange-panel follow-up — 2026-09-09

- Headset testing confirmed the numeric classifier worked, but the three orange
  background layers remained. Extended OFF to remove the complete native
  readout: panel layers, magazine icon, knife icon, and digits.
- Did not re-enable the rejected shader-wide panel suppression. RenderDoc
  confirms the ammo panel is a three-draw, six-index sequence followed by the
  HUD-exclusive magazine icon. That icon now grants a same/next-frame permit
  for only those panel shaders. When a native menu replaces the gameplay HUD,
  the marker disappears and shared menu panels fail open after at most the
  single transition frame.
- The magazine icon still reaches `NotifyGameplayHUDConfirmed()` before being
  skipped, preserving the gameplay-state latch. The shared knife/reticle shader
  additionally requires the armed magazine-to-digits sequence and the captured
  1024x1024 atlas, so neither the 32x32 reticle nor unrelated menu sprites are
  hidden.
- Release x64 built with 0 warnings and 0 errors. The focused menu guard, 2,893
  visibility checks, 21 adapter checks, native-camera ownership, watch fallback,
  embedded weapon calibrations, late-game cleanup, and prologue scripted-arm
  guards all pass. Candidate/source/archive/installed DLL SHA-256:
  `69AA0D9CDB5CF467DFAB2C4CF04F6550ECECE82CE816AEBDF56813C7F64B5E9A`.

### Scope Forced ADS menu toggle — 2026-09-09

- Connected the Controls-page checkbox, persisted as `scope_forced_ads`, with
  the current behavior preserved as the enabled/default state. Controls Reset
  and Reset All restore it to enabled.
- Split the existing combined condition into `wantScopeZoom` and
  `wantVisualScopeADS`. A positively identified 2x or 4x optic near the eye
  always requests its established projection magnification (1.744x or 3.488x),
  independent of the checkbox. Only Metro's native right-mouse ADS request is
  gated by Scope Forced ADS.
- Turning the option off while native visual ADS is active causes the existing
  state-transition path to send aim-up; keeping the weapon at the eye still
  leaves magnification active. Iron sights, reflex sights, and unknown optic
  configurations remain excluded from both forced visual ADS and scope zoom.
- Release x64 built with 0 errors and the existing third-party C4250 warning.
  The focused menu guard, 2,893 visibility checks, 21 adapter checks,
  native-camera ownership, watch fallback, embedded weapon calibrations,
  late-game cleanup, and prologue scripted-arm guards all pass.
  Candidate/source/archive/installed DLL SHA-256:
  `CEB01FF1D9EDC8A5FF45761ABEDBE94233EE6A82C92C5B1DE0D2B301F719F234`.

### Scope Sensitivity menu slider — 2026-09-09

- Confirmed the preceding menu/Forced ADS work did not alter the live trigger:
  it was still the headset-validated compile-time `0.12 m` vertical
  controller-to-HMD separation, with release at `0.18 m`. The inactive menu
  slider had merely displayed that same constant as a placeholder.
- Connected the slider over `0.05–0.30 m` in 0.01 m steps, persisted as
  `scope_sensitivity_meters`; the existing `0.12 m` value remains the default.
  Lower values require the hand to come closer to eye height, while higher
  values activate farther away.
- `IsWeaponNearFace()` now reads the live menu setting as its engage height and
  derives release as engage plus the established `0.06 m` hysteresis. Both the
  2x/4x projection zoom and optional forced visual ADS consume this one result,
  so their activation thresholds cannot diverge. The gesture deliberately
  remains a vertical-separation test rather than full 3D controller distance:
  the controller is naturally forward of the HMD when the scope itself is at
  the eye.
- Release x64 built with 0 errors and the existing third-party C4250 warning.
  The focused menu guard, 2,893 visibility checks, 21 adapter checks,
  native-camera ownership, watch fallback, embedded weapon calibrations,
  late-game cleanup, and prologue scripted-arm guards all pass.
  Candidate/source/archive/installed DLL SHA-256:
  `032FB9950B3CA8DD33DCB317522070B5AA3316340E129FA5BB93E5416CCEAF95`.

### Scope Sensitivity range/default follow-up — 2026-09-09

- Headset feedback found the `0.05 m` minimum useful enough to become the
  default but requested a closer endpoint. Changed the slider range from
  `0.05–0.30 m` to `0.01–0.30 m` and the default from `0.12 m` to `0.05 m`.
  The step remains 0.01 m, so every adjustment is visible in the two-decimal
  menu readout.
- No ADS/zoom routing changed: both still consume the same proximity result,
  and release remains engagement plus the 0.06 m hysteresis margin. Controls
  Reset and Reset All now restore `0.05 m`.
- Release x64 built with 0 warnings and 0 errors. Menu verification, 2,893
  visibility checks, 21 adapter checks, native-camera ownership, watch fallback,
  embedded weapon calibrations, late-game cleanup, and prologue scripted-arm
  guards all pass. Candidate/source/archive/installed DLL SHA-256:
  `498FF051F7C2F042C058E474FFC5FA2BBD96EA9CD79913D8EEEC29BB678BB163`.

### Scope-alignment height and minimum hysteresis — 2026-09-09

- Headset feedback exposed two limitations in the first live slider: its band
  was centered on the controller being level with the HMD even though the optic
  sits above the grip, making an unforced scope too high at activation; and the
  0.06 m release margin made the 0.01 m minimum feel much broader after zoom
  had already engaged.
- Centered the proximity band on a grip position 0.08 m below HMD height. The
  slider remains the tolerance around that scope-aligned height, so its 0.01 m
  endpoint now requires close vertical alignment while allowing the physical
  controller to sit naturally below the visible optic.
- Tightened release hysteresis from 0.06 m to 0.02 m. This retains a small
  anti-chatter margin without allowing the tightest setting to remain active
  far outside its engagement band. Forced ADS and projection zoom still share
  this exact result.
- Release x64 built with 0 warnings and 0 errors. Menu verification, 2,893
  visibility checks, 21 adapter checks, native-camera ownership, watch fallback,
  embedded weapon calibrations, late-game cleanup, and prologue scripted-arm
  guards all pass. Candidate/source/archive/installed DLL SHA-256:
  `638FA503EB5AB994806CA9B1BA40D90015E19C1DC009109CE28DC4AF3F5DDD68`.

### Scope Sensitivity true eye-proximity follow-up — 2026-09-09

- Headset testing at the new `0.01 m` minimum showed that the scope could still
  activate far from the face. The preceding correction constrained only the
  controller's vertical alignment, leaving an effectively infinite horizontal
  slab: any forward or sideways distance qualified at the correct height.
- Scope zoom and forced visual ADS now require both the existing grip-height
  alignment and a bounded 3-D distance from the eye-aligned grip point. Because
  OpenVR tracks the controller grip rather than the optic, the distance includes
  a `0.25 m` physical grip-to-optic allowance plus the selected sensitivity.
  Minimum therefore engages inside `0.26 m` and releases outside `0.28 m`; the
  `0.05 m` default engages inside `0.30 m` and releases outside `0.32 m`.
- The general raised-weapon result used for native recoil/dispersion control
  deliberately remains height-only. This preserves the earlier fix that stopped
  recoil suppression from dropping merely because the player extends the gun,
  while the 2x/4x scope path can no longer activate at unlimited reach.
- Release x64 built successfully. Menu verification, 2,893 visibility checks,
  21 adapter checks, native-camera ownership, watch fallback, embedded weapon
  calibrations, late-game cleanup, and prologue scripted-arm guards all pass.
  Candidate/source/archive/installed DLL SHA-256:
  `9DBB5DD2A6FED055A978CAC7AD2EC559816E45923164D283DE86A424F5DB470E`.

### Advanced weapon and global left-hand offset Apply — 2026-09-09

- Connected the twelve Advanced position/rotation sliders to the live transform
  values. Weapon and left-hand changes preview immediately in 0.002 m / 0.2
  degree steps, but slider movement itself does not write either calibration
  file. The displayed values now come from the active runtime calibrations
  instead of the old menu placeholder arrays and zero-valued left column.
- Added separate `APPLY WEAPON` and `APPLY LEFT HAND` rows beneath their
  respective columns. Weapon Apply is available only while a full weapon body
  was rendered recently and its stable profile identity is known; it passes
  that exact active key to the existing profile writer. This prevents a stale
  mesh set or a holstered/no-weapon state from modifying another gun, and leaves
  every other embedded/external weapon profile unchanged.
- Left Hand Apply deliberately remains global. The normal left-hand mesh uses
  one forced controller-relative pose for the whole game, independent of the
  equipped gun, so it writes the established `vr_left_hand_offset.txt` rather
  than creating per-weapon hand profiles. The separate prologue skeleton and
  its calibration file are not touched by this menu editor.
- Reset Advanced now previews zero values for both groups; the user must still
  press the corresponding Apply button to persist either group. Reset All keeps
  its existing settings behavior and does not silently rewrite calibration
  profiles.
- Release x64 built with 0 errors and the existing third-party C4250 warning.
  Menu verification, 2,893 visibility checks, 21 adapter checks, native-camera
  ownership, watch fallback, embedded weapon calibrations, late-game cleanup,
  and prologue scripted-arm guards all pass. Candidate/source/archive/installed
  DLL SHA-256:
  `D5AD83EEDAEF500BD41184967F3C957C99EB463D3310BF5CBEEAE33105B1207C`.

### VR-menu held melee release — 2026-09-09

- Headset testing found that the button used in the L3+R3 menu-opening hold
  could leave Metro repeating melee after the VR menu became visible. The menu
  branch released synthetic keyboard/mouse and left-stick state, but returned
  before `PublishPadButtons()` could replace the previous virtual XInput button
  mask. Metro therefore continued receiving the last held button state.
- The menu-open branch now explicitly publishes an all-buttons-up virtual-pad
  state on every frame. It also clears the inventory-held latch and both
  synthetic right-stick follow channels, while retaining the existing neutral
  left-stick, keyboard, mouse, firing, and aiming releases. The XInput hook sees
  the transition and increments its packet number normally, so Metro receives a
  real release rather than merely stopping future presses.
- Release x64 built successfully. Menu verification, 2,893 visibility checks,
  21 adapter checks, native-camera ownership, watch fallback, embedded weapon
  calibrations, late-game cleanup, and prologue scripted-arm guards all pass.
  Candidate/source/archive/installed DLL SHA-256:
  `26FC629B0CB6E163221C4F3ED4A68BEDC8A08B57F8568E8B8E3E9B052112D104`.

### Fixed-resolution VR-menu surface — 2026-09-09

- Headset testing showed that the first readability fix covered only the final
  compositor downsample. At VR resolution `1.00`, the isolated final-colour
  path was still disabled, so the menu itself was rasterized into Metro's
  current lower-resolution scene surface before submission and fine labels
  could remain unreadable.
- An open VR menu now activates the isolated per-eye final-colour path at every
  gameplay resolution scale. While open, its render target is fixed to the
  menu's established X2 reference canvas, `3620x2009`, rather than following
  the selected game scale. The completed game scene is resampled into that
  surface first and all subsequent overlay/menu draws land directly on it.
- Submission now explicitly selects the current-frame isolated surface whenever
  the menu is open. Closing the menu leaves the existing resolution behavior
  untouched: scales above `1.00` use their dynamically sized high-resolution
  scene target and scales at or below `1.00` use the proven native/downsample
  path.
- Release x64 built successfully. Menu verification, 2,893 visibility checks,
  21 adapter checks, native-camera ownership, watch fallback, embedded weapon
  calibrations, late-game cleanup, and prologue scripted-arm guards all pass.
  Candidate DLL SHA-256:
  `515734DBEB9CD4A7764C41AC948EA6574E08D78EE5036C1A8223C844F344F3F2`.

### Right-thumbstick resolution preset selection — 2026-09-09

- The Picture page's five VR Resolution Preset buttons were implemented in
  `Adjust()`, but the right-stick dispatcher admitted only rows classified as
  held adjustments. Because the preset row was excluded from that classifier,
  it could be activated only through Confirm and horizontal stick movement was
  ignored.
- Added the preset row to the right-stick adjustment set. A horizontal flick
  advances one step and a held direction uses the established 250 ms initial
  delay / 55 ms repeat cadence, clamped at Low and Max. Each preset continues
  to update both `resolutionPreset` and its exact scale value and save it for
  the next launch; no live render-graph rebuilding was introduced.
- Release x64 built successfully. Menu verification, 2,893 visibility checks,
  21 adapter checks, native-camera ownership, watch fallback, embedded weapon
  calibrations, late-game cleanup, and prologue scripted-arm guards all pass.
  Candidate DLL SHA-256:
  `2883C36EF8CED96D066C3ED161CDE80AD891B263945453424957539FFFE9CDD9`.

### Gamepad Mode retired from primary 6DOF menu — 2026-09-09

- The optional Gamepad Mode was determined to need its own native-viewmodel,
  ADS, head-aim, and startup behavior. Continuing to develop that hybrid inside
  the main DLL would increase regression exposure for the validated 6DOF
  weapon, hand, scope, projectile, and camera paths.
- Removed Gamepad Mode from the Controls page and reduced/reindexed that page
  from nine selectable rows to eight. Separate Hands, Left Handed Mode, Swap
  Analog Sticks, Scope Forced ADS, Scope Sensitivity, and Reset Controls retain
  their prior behavior and now use evenly redistributed row positions.
- The primary package now forces `gamepadMode=false` and disables the retired
  right-Y override after loading settings. If an older settings file contains
  `gamepad_mode=1`, startup immediately rewrites it as disabled. This prevents
  a persisted value from silently entering the hidden hybrid path.
- The underlying Gamepad implementation was deliberately left dormant in the
  source so its useful input/viewmodel work can be extracted into a separate
  Gamepad VR build without modifying the primary 6DOF runtime.
- Release x64 built successfully. Menu verification, 2,893 visibility checks,
  21 adapter checks, native-camera ownership, watch fallback, embedded weapon
  calibrations, late-game cleanup, and prologue scripted-arm guards all pass.
  Candidate DLL SHA-256:
  `0DDFFE37AAF9C0C756727AB76458B7FF327DB1D443E004493E60DCAA3672031A`.

### Separate Hands option retired from the primary menu — 2026-09-09

- Separate hand routing remains permanently enabled in the primary 6DOF build.
  Recombining the hands would cross several validated ordinary, post-clothing,
  prologue, late-game, scripted-hand, and physical-watch paths for little
  practical benefit.
- Removed the inactive Separate Hands row from the Controls page without
  changing any hand geometry, controller routing, pose, or watch behavior.
  The page now contains seven selectable rows; its remaining controls were
  reindexed and evenly redistributed, including the right-stick Scope
  Sensitivity adjustment and Reset Controls selection.
- The focused menu verifier passes and Release x64 builds with 0 warnings and
  0 errors. Source, archive, and installed DLL SHA-256:
  `67DCD6C6736F44F75BA8F9CC4578A06AA7702EA3A387FEE606F7B90BC0A58C5E`.

### Left Handed Mode retired from the primary menu — 2026-09-09

- The pre-separated-hands Left Handed Mode changed only the controller used as
  the weapon-pose source. The newer offhand geometry, physical watch, two-hand
  support grip, equipment gestures, and special hand families retain explicit
  physical-hand ownership, so exposing that partial toggle could route both
  visible hands to the left controller or break support-hand behavior.
- Removed Left Handed Mode from the primary 6DOF menu. The established
  right-handed hand and weapon paths are unchanged, while the dormant setting
  and controller-selection code remain available for reuse in the planned
  native-style Gamepad VR DLL.
- Startup forces the hidden setting off and rewrites older settings files that
  contain `left_handed_mode=1`, preventing a saved partial mode from remaining
  active after its menu control is removed.
- The Controls page now has six evenly distributed selectable rows. Swap Analog
  Sticks, Scope Forced ADS, Scope Sensitivity, and Reset Controls were reindexed
  together so their selection highlights and right-stick adjustment remain
  aligned with the rendered controls.
- The focused menu verifier passes and Release x64 builds with 0 warnings and
  0 errors. Source, archive, and installed DLL SHA-256:
  `535BA817A1CEF41ACAC7770967D798A2ADDF3665A233BEBBC256F7CF2FEC5D29`.

### Independent Quality page scaffold — 2026-09-09

- Headset testing established that the VR fixes currently require Metro's
  Medium master-quality preset. Other master presets load different shader
  packages/render passes and invalidate exact shader and draw signatures used
  by the validated VR routes.
- Static inspection confirms that Metro persists only `r_quality_level` as its
  public master control. The executable contains separate internal controls
  including `r_lod_shadow_quality`, `r_lod_light_srange`,
  `r_lod_light_urange`, and `r_lod_shader_range`, plus four precompiled shader
  package variants. It also contains an explicit renderer warning that reset is
  impossible after changing quality/DOF/tessellation. No equivalent persistent
  independent texture-tier variable has yet been confirmed.
- Added Quality between Picture and Controls without exposing Shader Quality.
  The page presents Textures, Lighting, and Shadows with Low, Medium, High, and
  Very High choices, a Medium compatibility warning, and Reset Quality. The
  requested selections persist in `vr_menu_settings.txt`, default to Medium,
  clamp safely on load, and support horizontal right-stick adjustment.
- These three controls are deliberately marked as pending and do not change
  Metro's renderer yet. Each underlying mapping must be connected and tested
  independently so the master quality/shader package remains Medium and the
  established VR shader signatures are not disturbed. Controls, UI, and
  Advanced were shifted to their new tab indices without changing their
  behavior.
- The focused menu verifier passes and Release x64 builds with 0 errors and the
  existing third-party C4250 warning. Source, archive, and installed DLL
  SHA-256:
  `C844E6A55C2269B1F0C05F4F4744F76C91BA735827578FD5C4181889207D49CD`.

### Independent Shadow Distance control — 2026-09-09

- Connected the first package-independent Quality control without changing
  Metro's required Medium `r_quality_level`. The executable's separate
  `r_lod_shadow_quality` value is exposed as a continuous 0.25-2.00 Shadow
  Distance slider instead of claiming equivalence with the master presets.
  Static registration data defines an approximately 0.001-2.00 engine range;
  the menu retains a safer 0.25 floor, while 1.00 is the native/default Medium
  behavior. Lower values reduce distant shadow coverage for performance and
  higher values extend coverage without changing the shader package.
- Shadow changes are deliberately restart-only. The menu updates `user.cfg`
  and schedules one hidden post-exit rewrite from the final value in
  `vr_menu_settings.txt`, preventing Metro's shutdown cvar save from erasing
  the selection. No live renderer reset is requested and no shader package is
  changed.
- Texture and Lighting remain visibly disabled and no longer respond to
  Confirm or horizontal stick input. Their saved scaffold fields are retained
  for later validated mappings.
- The relevant regression risk is earlier shadow cutoff/pop-in at reduced
  slider values. This control does not alter the view-space matrix corrections
  that fixed moving shadows, but those scenes should still be included in the
  headset test before choosing a reduced default.
- The focused menu verifier passes and Release x64 builds with 0 errors and the
  existing third-party C4250 warning. Source, archive, and installed DLL
  SHA-256:
  `172A1B11FFB5F77DEDACD987512A24CCBA929DDB2C14ABAFCFCD7A4EF4EFFC6B`.
  Headset/restart validation is pending. The prior 1.00-capped DLL is preserved
  as `Builds/vr-menu-revamp-20260909/d3d11.shadow-distance-1-max.dll`, and the
  original disabled-shadow scaffold remains available separately.

### Independent Lighting Distance control — 2026-09-10

- Static registration data identifies `r_lod_light_srange` as a continuous
  10-100 control with the Medium runtime value at 66, and
  `r_lod_light_urange` as a continuous 10-200 control with the Medium value at
  125. These affect shadow-casting and unshadowed light visibility ranges
  without selecting the `_high` or `_vhigh` shader packages.
- Replaced the disabled Lighting tier placeholder with a 0.50-1.50 Lighting
  Distance slider. Its 1.00 midpoint preserves 66/125; 1.50 produces 99/187.5
  and therefore remains inside both registered engine maxima. Lower values
  trade light range for performance.
- The setting uses a new `lighting_distance_scale` key and deliberately ignores
  the old presentation-only `lighting_quality` value. This migrates accidental
  scaffold selections, including the user's saved zero, to the neutral 1.00
  default rather than silently lowering active lighting.
- The existing restart-only native-cvar synchronization now writes Shadow
  Distance and both light ranges together, including the post-exit rewrite
  needed to survive Metro's shutdown config save. Master `r_quality_level`
  remains untouched and Texture stays disabled.
- Release x64 builds with 0 errors and the existing third-party C4250 warning;
  the focused VR-menu verifier passes. Source, archive, and installed DLL
  SHA-256: `7A6172202C280578D626911407F67553C857060F77650E55F2CA5F3B67D1DD11`.
  Headset/restart validation is deferred until convenient for the user.

### Quality page retired — 2026-09-10

- Headset A/B testing of Lighting Distance at 0.50 and 1.50 across full process
  restarts produced no visible difference. The underlying cvars only affect
  distant light eligibility, not nearby lighting appearance, shader quality,
  shadow filtering, or texture detail, so the control did not fulfill the
  intended purpose of a user-facing Quality option.
- Removed the complete Quality page and restored four-page navigation:
  Picture, Controls, UI, and Advanced. Tab spacing, row counts, selection
  indices, held-slider behavior, reset routing, and Advanced's explicit-Apply
  persistence guard were shifted together.
- Removed the Texture placeholder, Lighting Distance and Shadow Distance
  settings, native `user.cfg` rewrite code, and post-exit PowerShell helper.
  The experimental `r_lod_shadow_quality`, `r_lod_light_srange`, and
  `r_lod_light_urange` lines were removed from the active game config while
  preserving `r_quality_level 3` unchanged. Obsolete quality keys were also
  removed from `vr_menu_settings.txt`.
- Backups of both pre-cleanup configuration files and each experimental DLL
  remain in `Builds/vr-menu-revamp-20260909`. The stable menu no longer owns or
  modifies any Metro quality cvar.
- The focused VR-menu verifier passes and Release x64 builds with 0 errors and
  the existing third-party C4250 warning. Source, archive, and installed DLL
  SHA-256: `5D612DB8266B068513C6EA07D3A05580A798B46663DC3F62412E75EC9D1663A0`.

### Reticle depth control removed — 2026-09-10

- Removed the UI-page Position Z slider because the reticle is constructed as
  an infinite-distance sight ray. No renderer consumed the saved third
  component, and giving it finite depth would add binocular disparity and make
  the reticle accurate at only one chosen distance.
- Renamed the two functional controls from Position X/Y to Horizontal Aim and
  Vertical Aim to reflect their actual angular sight-zero behavior. Size, Ammo
  Counter, Reset UI, selection navigation, held adjustment, and highlight
  geometry were all shifted to the six-row layout together.
- The legacy `hud_position` third component remains in the settings format for
  backward compatibility and is forced to neutral zero whenever the UI page is
  adjusted. The focused verifier rejects a visible Position Z row and checks
  the new labels and indices.
- Release x64 builds successfully and the focused VR-menu verifier passes.
  Source, archive, and installed DLL SHA-256:
  `695A33E03C14CB9F3112B88A2A17AF3716C2578ECCE86CD4E9405BFE2B2CF85A`.

### VR menu release cleanup and clean-install embedding — 2026-09-10

- Removed the disabled `kADSHandHeightTrace` scope-tuning block and added a
  release guard that rejects its diagnostic identifiers. Other enabled hooks
  with legacy `Probe` names are validated camera/shot-routing mechanisms from
  the accepted base build, not temporary logging paths, and remain unchanged.
- Audited the active game's calibration and learned-data files against compiled
  defaults. Four tested dependencies were still file-backed: the global normal-
  game left-hand offset, normal watch assembly offset, journal surface
  calibration, and 82 unique learned viewmodel index counts. Embedded their
  exact headset-validated values into the DLL.
- The existing 11 embedded weapon profiles cover all 14 calibrated weapon
  variants through shared profile families. A focused test now verifies those
  profiles plus the 82-mesh table and the left-hand, watch, and journal defaults.
  External calibration and mesh files remain optional, additive/last-wins
  overrides so Advanced Apply and future per-user calibration continue working.
- Fresh installs therefore reproduce all completed placements and immediate
  viewmodel classification without shipping machine-local learned files. The
  normal settings file remains intentionally external because it stores user
  choices; Enhanced Blacks and Expanded Visibility both compile as enabled.
- Release x64 builds successfully. All 17 self-contained Python test programs,
  four PowerShell release guards, and nine compiled C++ harnesses pass. This
  includes 2,893 visibility-coverage checks, 21 adapter checks, menu behavior,
  native camera ownership, late-game hands/lasers/flame, prologue arms, watch
  fallback, and embedded calibration coverage. Final DLL SHA-256:
  `3FFB3A03600F841D42C6E6FB11B3BD5FC8BD6BE55F88BEE182FE98C2A334FCD7`.

### Archives watch and invisible enemy — headset accepted 2026-09-10

- User confirmed both detached watch children (visibility lamp and cover) were
  fixed by giving Spartan children the exact completed same-frame 6,108 casing
  matrix. Illumination was not a reliable reproduction condition; user found
  the displacement particularly reproducible over water.
- User subsequently reported an enemy visible outside VR but invisible in VR.
  Disabling global bone-palette rewriting did not help. Disabling all three
  special hand-family classifiers restored visibility, but did not isolate
  the individual responsible classifier.
- Geometry fingerprints and viewport gating did not fix the disappearance.
  The earlier assertion that fingerprint C7940172515E46D9 proved the 42,516
  mesh belonged to the player was incorrect: it proved only that the same
  previously captured asset was present. That original capture was in the
  world pass with translation (-3.393733, -1.563496, 26.821623).
- Disabling only the 42,516 body-parent route restored enemy visibility. User
  separately confirmed that hands and watch remained correct. This supersedes
  the September 7 notes claiming that world mesh was a required player/watch
  parent. Do not restore its suppression or parent publication based on its
  index count, shader pair, or saved geometry fingerprint.
- Accepted build retains separated visible hands (17,784), post-clothing hands
  (21,762), their added geometry checks, global palette rewriting, and the
  watch child matrix fix. `kEnableUnverifiedLateBodyParent` remains false.
- Release x64 built successfully; installed DLL hash rechecked after user
  acceptance: `2659DBA85600071EA9AAEC45A3005C2D12C59CA50C29F384BB4852BECB1E8CA3`.
  Archive: `Builds/archives-enemy-only-42516-route-disabled-20260910`.
  No further runtime changes were made after headset acceptance.

### Archives release cleanup — 2026-09-10

- User additionally checked Armory and a couple of later stages and confirmed
  hand presentation was correct with the accepted fix.
- Removed the rejected 42,516 classifier, its disabled experiment switch,
  fingerprint admission, and unreachable body-suppression/watch-parent branch.
  The enemy now follows ordinary world rendering. Earlier notes asserting that
  this captured mesh was the player's body are superseded by the isolated test.
- Removed this session's watch census, route logging, child-isolation branch,
  unused anchor-trace variable, and geometry fingerprint logging. Historical
  captures and diagnostic build archives remain available as evidence/rollback;
  they are not runtime dependencies or release payloads.
- Retained the accepted 21,762 and 17,784 geometry checks and the latter's
  viewmodel gate. Both reference fingerprints are compiled into the DLL.
  Watch children reuse the completed same-frame casing matrix and all private
  watch draws start at instance zero. Startup suppression remains limited to
  the observed Archives watch material signatures. These fixes need no new
  external settings or capture files; no additional embedding was required.
- Updated hand/prologue release tests for the removed body branch and retained
  watch guards. Release x64 builds; all four PowerShell release guards and six
  focused Python checks (hand topology, prologue routing, release cleanup,
  embedded calibrations, Market hands, town watch fallback) pass. Verified that
  this session's diagnostic identifiers and rejected body route are absent.
- Cleanup DLL and PDB deployed and hash-verified with Metro closed. DLL SHA-256:
  `DE1F896067AF9C8ED0CC88C48456563FB6BCEE469E09FEE653041C4AF725F6E0`.
  Archive: `Builds/archives-issues-release-20260910`, including the preceding
  headset-accepted DLL/PDB. Cleanup build has automated validation; the user's
  headset acceptance applies to its preceding behavior-equivalent build.

### Prologue watch rejected attachment experiments — 2026-09-10

- User repeatedly reports the watch moves far from its calibrated position;
  the latest visible-cuff candidate is rejected, not accepted or fixed.
- Removed the 135-line visible-cuff experiment from HackerContext.cpp. Restored
  the pre-experiment DLL and matching baseline PDB with Metro closed. Verified
  installed DLL SHA-256:
  `F8BB69AE2B9079C7E80D86619434F5825AC7207ADB5DB3D58E048E4A364FEC61`.
  This rollback does NOT fix the original prologue locomotion/journal separation.
- Preserved the saved prologue watch calibration unchanged:
  `-0.450301 0.247900 -0.205200 0.522923 -0.332777 -0.148408`.
- Relevant code difference: the prologue keeps its live hand bone palette;
  ordinary gameplay substitutes its preferred hand pose. The watch uses a
  fixed wrist reference. Identical calibration handling alone does not make
  these two visible hand motions identical.
- Both latest rejected attachment candidates used eight stable startup frames
  to capture a permanent hand/watch relationship. That gate does not establish
  that the hand is in the pose in which the user calibrated the watch.
  The earlier skinning measurement has identity hand54 at frames 1170–1230,
  followed by a substantially rotated/translated hand54 at frame 1260.
  The latest rejected runtime logs its attachment lock before scripted
  pass-through engages at frame 1434. These are separate runs: this supports
  investigating premature capture, but does not prove the exact captured
  palette in the latest run. Do not present it as a confirmed root cause.
- The sampled watch bone3 was identity throughout the 73-sample measurement;
  earlier claims that watch skin animation caused a double transform were
  unsupported. Do not repeat them as evidence.
- Preserve existing build/capture archives. Do not deploy another attachment
  candidate based only on a successful build or an algebraic identity at the
  capture frame. Verify the reference pose and preservation of the calibrated
  gameplay placement before asking for another headset test.

### Explicit idle reference capture prepared — 2026-09-10

- User confirms the restored build's watch placement is correct while standing
  still with the journal closed; movement/journal causes separation. This was
  already demonstrated in the video. Do not ask the user to reconfirm it.
- No complete verified prologue left-hand palette was found in saved captures.
  The two-bone skin trace cannot supply the entire 19-bone preferred pose.
- Installed a capture-only build, not a fixed-pose candidate. It reads the
  already-available palette without changing it, only on the prologue gameplay
  route outside scripted ownership. No watch or calibration changes.
- On the user's next READY, while they remain standing still with journal
  closed and the original correct placement visible, use apply_patch to create
  `<METRO_INSTALL>/vr_prologue_idle_reference.request`.
  Do NOT create it before that confirmation: startup capture was the prior flaw.
  The running build checks once per second and writes one CREATE_NEW file,
  `vr_prologue_idle_reference.capture`, without overwriting existing evidence.
  Header is four little-endian uint32s: magic 0x50495231, version 1, frame,
  float count. The complete native b8 float palette follows, before any hand
  substitution. Check log success, exact file length, finite values, and skin
  reconstruction before embedding a prologue-only fixed pose. This capture
  does not itself change presentation, including on subsequent launches.
- Release x64 succeeded. Seven Python checks passed, including the new
  `Tools/test_prologue_reference_capture.py`: removing the marked capture block
  yields HackerContext.cpp exactly as in c5b342a. Scripted arms, later hands,
  release cleanup, Market hands, town clock, and embedded calibrations pass.
- Archive and rollback DLL/PDB: `Builds/prologue-explicit-idle-reference-20260910`.
  Deployed DLL/PDB hash-verified with Metro closed; DLL SHA-256:
  `B84F8B517F087796F8A0266E460F47628B4926788E487A611BBA4900EA63CB55`.
  Both saved prologue calibration files verified unchanged. No request file
  has been created yet. Awaiting the user's ready signal, not a fix verdict.

### User-selected prologue idle pose candidate — 2026-09-10

- User said ready while standing still with the journal closed and the restored
  calibrated hand/watch placement visible. Created the request then, not during
  startup. Capture succeeded at frame 3246, 964 floats (3872 bytes with header).
  Evidence: `GameReferences/prologue_calibrated_idle_20260910/idle.capture`;
  SHA-256 `3A8B8A9636DFDEEF5CDB24CF59874A808C853FAA02E17376066146268D2CBA63`.
  Full runtime log and unchanged calibration files archived beside it.
- Embedded the 19 prologue left bone entries (row bases 0..54, step 3) in
  `EmbeddedPrologueLeftHandPose.h`. Apply only to the isolated prologue left
  hand during gameplay outside scripted performance, before wrist evaluation.
  Like the normal preferred-pose path, this removes walking/journal skin motion
  from the controller-driven hand. It does not make the watch follow the old
  journal animation; it keeps the hand in the selected pose beside the watch.
- Watch matrices/reference, calibration handling, all other skeletons, native
  right-hand palette, stereo restoration, and full scripted-arm routes remain
  unchanged. Removed the temporary capture code and its temporary test. Moved
  the request to `captured.request` in the reference archive (recoverable).
  The new runtime needs neither request nor capture files; pose is embedded.
- `Tools/test_prologue_fixed_idle_pose.py` verifies float32-exact embedding of
  all 228 values, unchanged selected-frame palette, reconstruction of all 1344
  drawn left vertices with zero difference, and left-only substitution under
  arbitrary animation input. Removing the new block/include/comment change
  reconstructs c5b342a HackerContext.cpp exactly, guarding unrelated routes.
  Six other focused checks pass (scripted arms, late-visible split, release
  cleanup, Market hands, town clock, embedded calibrations). Release x64 builds.
- Deployed with Metro closed and DLL/PDB hashes verified. DLL SHA-256:
  `B0721EB941B748BD990DD089B4791CE2E79F4B896AFFE74D63DB2541DAC55C05`.
  Archive: `Builds/prologue-fixed-selected-idle-20260910`, including non-diagnostic
  F8BB69 rollback DLL/PDB. Both prologue calibration files hash-match the capture.
- This is an automated-validated candidate, NOT headset accepted. Next checks:
  original placement, walking/running, journal open/close, restart persistence;
  then scripted prologue presentation and a known-working later hand/watch.

### Prologue journal lighter fixed-pose transport candidate — 2026-09-11

- User confirms the fixed hand pose keeps the watch attached. The journal
  lighter stays detached even after opening finishes. Preserve the hand/watch
  change; do not return to moving the watch or guessing calibration offsets.
- The former lighter local capture simplified to inverse(B_visible), where B
  is the hand's model-space aggregate anchor. With the newly frozen palette,
  that captures inverse(B_fixed), cancelling the pose change instead of
  transferring the existing lighter calibration from the authored hand pose.
- Added an unfrozen model-space aggregate anchor evaluated from livePosePalette
  alongside the unchanged fixed visible anchor. Only journal variant 3 now
  captures inverse(B_authored) at the existing closed-root gate and consumes it
  under the fixed visible anchor. Hand/watch matrices, calibration files,
  lighter root/lid correction, flames, all other variants and routes unchanged.
- New test verifies hand-relative rigid transport, equivalence when authored
  and visible poses coincide, and exact source preservation outside the three
  changed regions against the watch-accepted candidate. Full 1344-vertex idle
  pose replay still passes, along with six existing focused guards. Release
  x64 builds and DLL/PDB deployed hash-verified with Metro closed.
- Artifact: `Builds/prologue-journal-fixed-pose-transport-20260911`; rollback is
  the user's watch-accepted B0721E build. Candidate DLL SHA-256:
  `DBB250DCFB1EEA79F814DE02F88EF10DAA5EC61217589CDAA355D04DE8CC1B06`.
- Awaiting headset verification, not accepted. Pre-change runtime is archived:
  it has generic journal root locks at frames 2536 and 4957 without the
  prologue aggregate lock message, and normal preferred-hand initialization.
  Thus the exact route during the reported failure is NOT conclusively proven.
  If this candidate does not affect it, inspect the active hand/variant rather
  than making further calibration or attachment guesses. This candidate only
  corrects the demonstrable prologue pose-reference mismatch.

### Both prologue lighter placements rejected; measurement — 2026-09-11

- User rejects the journal transport and reports BOTH regular and journal
  lighters opening away from their previously calibrated positions. No new
  calibration is authorized or needed as a substitute for fixing the regression.
- Verified both current embedded six-value prologue body calibrations EXACTLY
  match the archived accepted 2026-08-24 files. No game-folder overrides exist;
  their removal was intentional release cleanup, not lost calibration.
- Latest runtime DOES log the prologue aggregate lock at frame 5029 and later
  regular lighter draws alongside prologue WRIST_LOCKED frames. Earlier routing
  uncertainty is not sufficient to explain this rejected test. Archived at
  `Builds/prologue-journal-fixed-pose-transport-20260911/rejected-runtime.log`.
- Removed the rejected authored/fixed transport and its test. Restored the
  watch-accepted B0721E code and DLL, preserving the frozen hand. Then added
  bounded measurement only: max 20 samples for each prologue lighter variant,
  at least 60 frames apart. Logs calibration, final hand/lighter/visible-anchor
  matrices, all 19 unfrozen hand bones, corrected lighter bones and closed root.
  No rendering, calibration, bone, or particle state is changed by measurement.
- `test_prologue_fixed_idle_pose.py` strips the three marked measurement blocks
  before checking exact baseline equivalence and all 1344 vertex positions.
  It and six existing guards pass; Release x64 builds. Installed DLL/PDB were
  hash-verified with Metro closed. Archive/rollback:
  `Builds/prologue-both-lighter-placement-measurement-20260911`.
- Awaiting ONE measurement run: open journal for several seconds, close it,
  turn on standalone lighter for several seconds, then quit and say done.
  No recalibration and no fix verdict requested. Parse `VRPose lighter measure`
  records before any further placement candidate. This is NOT a completed fix.

### Shader-scale prologue lighter attachment candidate — 2026-09-11

- Measurement completed: 12 journal samples, frames 1598..2258; root lock is
  active from sample 1658. Regular lighter draws are logged at frames 2725+
  alongside prologue hand draws, but no detailed variant-2 sample passed the
  diagnostic's same-frame hand requirement. Do not claim a full regular trace.
  Archived full runtime under the measurement build and candidate below.
- Found a concrete geometry mismatch. Prologue centre/wrist coefficients were
  derived from positions divided by 4096. The exact prologue hand shader
  79BA9F1ECFAF0CA1 decodes SINT POSITION at 0.000366210938 = 12/32768,
  i.e. 1.5 times that scale. Verified shader source archived as
  `hand-lighter-native-vs.txt`. Do not scale bone translations with vertex data.
- Measured fixed-pose model centre from the old proxy is
  (-.04955,-.25431,.66524); shader-accurate centre is
  (.09655944,-.13572232,.74573846). At journal sample 1658 the live proxy is
  (-.27450,-.11515,.69084), while shader-accurate live centre is
  (-.20645,-.18571,.82429). Saved journal lighter centre remains
  (-.148426,-.144054,.822864). This disproves treating the previous proxy as
  the actual visible glove centre; previous zero-error tests compared poses
  at the same incorrect scale, not the shader's geometry scale.
- Candidate keeps the calibrated hand/watch solve and legacy anchor untouched.
  It additionally evaluates fixed/live physical hand anchors at shader scale
  for the two prologue lighter variants only. Regular lighter transports its
  existing authored hand-relative relationship to the fixed physical hand;
  journal uses its existing closed-root capture gate with the accurate authored
  model anchor. Both use the same completed hand frame (including the existing
  permitted previous-frame parent), avoiding a new same-frame-only assumption.
  No saved offsets, other-level routes, or lid/flame code changed. Temporary
  measurements removed.
- Tests: actual shader literal, position-only scale correction, calibrated-local
  transport/no-change identity, exact source preservation outside four allowed
  regions against B0721E, and 1344-vertex replay at the actual shader scale pass.
  Six existing focused guards pass; Release x64 succeeds. Mathematical tests
  do not establish headset acceptance or prove the regular draw's full motion.
- Installed with Metro closed; DLL/PDB hashes verified. Candidate archive:
  `Builds/prologue-lighters-shader-scale-20260911`, including B0721E rollback.
  DLL SHA-256:
  `C43B7BE910255CFC4B197676D09E1FCEE96457A47814D67D53AC5475A9561873`.
- Awaiting headset check of BOTH saved lighter placements; no recalibration.

### Lighter placement accepted as sufficient; journal close input — 2026-09-11

- User reports lighter placement is not exactly the historical calibration but
  is good enough. Preserve C43B7B lighter/hand/watch behavior; do not describe it
  as an exact restoration of the old placement or continue changing offsets.
- New report: LT+L3 closes the journal and then reopens it. Code sends a 108-tick
  synthetic BACK hold for both opening and closing, regardless of physical
  release. That long closing hold is the likely native reopen cause, not yet
  proven by native input instrumentation.
- Added JournalButtonPulse: preserve 108-tick opening hold; use a six-tick
  close press based on the existing IsJournalPresented state. Require BOTH
  LT and L3 released after a completed pulse before rearming; partial release,
  threshold jitter, or holding the chord cannot emit a second pulse. VR menu
  cancellation clears the pulse and requires release before rearming.
- Scope: only journal pulse generation in PublishPadButtons. Existing rising
  edge notification, equipment/menu controls, hands, watch, lighters, saved
  calibrations unchanged. Presentation state is still the existing synthetic
  toggle; alternate native closures/state desync are not addressed here.
- Compiled and ran Tools/test_journal_button_pulse.cpp against the actual C++
  helper. Opening duration, short close, held controls, partial release/jitter,
  overlapping press attempts, and menu cancellation pass. Hand geometry,
  lighter scale/transport, scripted arms, and embedded calibration guards pass.
  Release x64 succeeds. DLL/PDB deployed and hash-verified with Metro closed.
- Archive/rollback: Builds/journal-close-short-pulse-20260911. Rollback retains
  user's sufficiently accepted lighter placement. Deployed DLL SHA-256:
  `ECB57C67201CB82281D790EB74F2F404047DDA82C459EBB34EF24EB4900B6C16`.
- Awaiting in-game LT+L3 open, full release, LT+L3 close verification. The
  six-tick close duration is a candidate, not yet headset-accepted.

### Journal close accepted; shared native journal grip candidate — 2026-09-11

- User confirms journal close/reopen issue is fixed. Preserve ECB57C input.
- Reviewed Video Project 76.mp4 (40.9 sec, four journal openings). Prologue
  hand grips the journal top; later sections show displaced/differently posed
  right hands relative to the book. Review frames under Builds/journal-video76.
  User requests matching grip/position across hand changes, retaining chapter
  hand models, and explicitly approved a journal-only change after warning of
  weapon/charger/transition and page-text regression risks.
- Existing split-hand path treats no recognized weapon body as an empty right
  hand, applying independent hand calibration and sometimes its fixed empty
  pose. The journal/text retain the viewmodel weapon parent. Candidate excludes
  journal presentation from that empty-hand handling, keeping native journal
  grip bones and the normal shared viewmodel parent. One common rule covers
  12132, 21762, and 17784 split families. Prologue routing stays unchanged.
- JournalHandRouting is stateless and charger-prioritized. Left-hand routing,
  hand meshes/ranges, existing weapon transforms, page text, input, watch and
  lighter code are unchanged. Uses existing IsJournalPresentationActive state;
  native transitions/alternate closures still require headset verification.
  This does not copy prologue bone indices onto incompatible skeletons or
  globally change weapon calibration. No claim of exact visual match yet.
- Compiled C++ routing test covers all 32 boolean combinations and repeated
  enter/exit across three family cases. Scope guard reverses only this small
  change to exactly recover the prior HackerContext source and verifies VRPose
  remains byte-identical to the journal-close build. Hand/lighter replay,
  scripted arms, late split, Market, town clock, calibration, flamethrower and
  release cleanup guards pass. Release x64 builds.
- Archive/rollback: Builds/journal-native-grip-shared-route-20260911. Installed
  DLL/PDB hash-verified with Metro closed; rollback retains accepted journal
  input and sufficiently accepted lighter placement. Awaiting the four-section
  headset check plus return to weapon/charger/empty hand after closing.

### First journal opening after first level change: diagnostic only — 2026-09-11

- User clarifies shared grip works except FIRST journal opening in SECOND
  level tried in a fresh process, regardless of chapter. Correct hand appears
  during closing; reopening there and all later levels work. Do not treat as
  a particular mesh/chapter exception or a failure on every level load.
- Code inspection: IsJournalPresented is only a synthetic BACK rising-edge
  toggle; it has no loading reset or reconciliation with native journal draws.
  This can explain an inverted route but does not prove the specific one-time
  sequence. No speculative reset, pose, input, or calibration fix applied.
- Installed bounded diagnostic ONLY: loading-panel episode boundaries, journal
  button before/after state, qualified physical page episodes/state changes,
  and shared right-hand route changes (mesh, weapon absence, journal state,
  charger, late-family, independent parent, freeze selection). Limits per
  process: 64 load, 64 button, 128 page, 256 route reports. Prefix is
  `Journal lifecycle`. No second-level counter changes behavior.
- Exact source guard removes four marked diagnostic blocks and recovers the
  previous installed source (VRPose matches accepted close archive; HackerContext
  matches shared-grip archive). Existing pulse/routing C++ tests, hand vertex
  replay, lighter transport, calibration and eight focused guards pass. Release
  x64 succeeds. These are not runtime proof of the cause or a fix.
- Metro closed; previous installed DLL hash confirmed before deployment;
  DLL and PDB copied and hash-verified. Archive and full DLL/PDB rollback:
  `Builds/journal-second-load-trace-20260911`. Prior log preserved there.
  Installed DLL SHA-256:
  `F5C90E08646DB942E96BD6804BE815B361C486EB4DD80AB3D125A944AC4EE018`.
- Next: fresh launch, reproduce normal first level then second-level first
  journal opening, close/reopen once, quit and inspect the trace. No
  recalibration requested. Remove temporary trace after diagnosis.

### Text-only journal bob: measurement, not a fix — 2026-09-11

- User confirms text alone bobs in every hand section; book and other parts
  are steady. Preserve all accepted placements and existing close input.
- Reviewed August 20 history before editing: full native-page locking did NOT
  eliminate the text bob; controller-derived parent translation worsened
  tracking. Do not repeat these as if they were proven solutions.
- Current text chain still uses native page, page-local calibration, cached
  weapon affine, then per-eye view correction and projection. Need measurements
  to separate these contributions under current steady-book behavior.
- Installed observation-only block in BuildJournalScreenMatrix: eye 0, one
  sample per six frames, max 300 samples per process. Captures full native,
  calibrated, parent, parented matrices, view correction and right-controller
  head-relative pose; includes parent frame/eye and pose validity. Prefix:
  `Journal text motion`. No GPU readbacks, transform changes or locks.
- Existing second-load lifecycle trace retained. Successful two-opening run
  preserved at Builds/journal-second-load-trace-20260911/
  successful-run-20260911-130441.log: correct routes, no loading notification.
- New scope test removes only measurement block to recover prior installed
  HackerContext exactly; VRPose identical. Existing routing scope, pulse-source
  preservation, physical lighter replay and 1344 hand-vertex replay pass.
  Release x64 builds. Metro closed; expected prior DLL checked; DLL/PDB deployed
  and hashes verified. Archive/rollback: Builds/journal-text-motion-trace-20260911.
  DLL SHA-256: BFB14DA34418CCFDE8391A8104C8BDD699F3104BE72B374DBF53FFA94A1A5738.
- Requested capture: any one level, open journal, hold controller/head steady
  about 5 seconds, walk about 5 seconds, rotate journal about 5 seconds, quit.
  This build does not remove bob; capture needed before a behavior candidate.

### Text motion replay results — 2026-09-11

- User completed capture: text independently bobs at idle, gains independent
  walking movement, and otherwise rotates correctly. Requested rigid page
  attachment rather than a separate text movement.
- Preserved captured-idle-walk-rotation.log under journal-text-motion-trace
  archive. 300 samples, frames 2636..4430; recorded parent is fresh eye 0.
  Tools/analyze_journal_text_motion.py parses matrices and replays composition;
  maximum parent*calibrated versus recorded parented error is 1.46e-7.
- After draw-in, idle native centre range is approximately .0022/.0059/.0003
  game units. During walking it reaches roughly .036/.026/.002. The live
  controller-local text position also varies: this is not just tracked motion.
- Counterfactual replay holds the calibrated native matrix at sample 30 (frame
  2816), while keeping EVERY recorded parent, view-correction and controller
  matrix live. Residual controller-local ranges: idle .000056/.000062/.000032;
  walking .000165/.000361/.000137. Approximately 99% reduction in the main
  walking residual. This supports native-only stabilization for CURRENT build,
  unlike the historical failed build. It is numerical evidence, not headset
  proof; capture limit may not include the entire final rotation segment.
- No rendering changes or deployment this turn. A native-page stabilization
  candidate still needs a safe open/close/reference lifetime design. Risks:
  capturing draw-in/walking phase can offset text; holding through draw-out can
  detach it during closing. User requested warning before regression-risky
  candidates. Do not change shared weapon transforms or calibration values.

### Text-only stable native page candidate installed — 2026-09-11

- User explicitly approved candidate after opening/closing/reference risks.
- JournalPageStabilizer applies ONLY to the private local nativePage inside
  BuildJournalScreenMatrix, before unchanged calibration and live weapon/eye
  transforms. No game buffers, physical book, hand, watch, lighter, shared
  weapon transform or VRPose input code changed. Existing traces retained.
- Acquires a mean native reference after 90 frame span and >=12 distinct frame
  samples within .008-unit centre extents and axis dot >=.9998. Opening motion
  resets acquisition. Once acquired, walking cannot alter reference. Opening
  while walking may remain live until the user briefly stops; this is a safety
  fallback, not an assertion of unconditional first-frame stabilization.
- Same-frame calls reuse reference only when locked; otherwise preserve each
  incoming matrix. A >12-frame draw gap drops the reference. Any new journal
  button frame after acquisition releases lock and suppresses reacquisition
  through remaining draw-out, independent of the unreliable inferred-open
  toggle. Unsignaled native closures still need headset validation.
- C++ tests exercise draw-in rejection, settling, walking hold, stereo reuse,
  close, reopening/gap, invalid data and duplicate-frame calls. Replay uses
  actual compiled helper, not a Python replacement: captured run locks at frame
  2744 (sample18). Controller-local centre residual reduction 98.62% idle and
  98.92% walking, with calibration/parent/view chain live. Reference is exactly
  constant after acquisition. This is NOT headset acceptance or proof of all
  chapter/closing paths.
- Scope guard reverses only include and three-line native-page integration to
  recover preceding installed HackerContext source exactly; VRPose unchanged.
  Existing journal pulse/routing C++ tests, hand/lighter replay and all eight
  focused regression guards pass. Release x64 builds successfully.
- Metro closed; previous DLL checked, rollback DLL/PDB and prior log saved;
  deployed DLL/PDB hash-verified. Archive: Builds/journal-text-native-stable-20260911.
  DLL SHA-256: DF09E6F8A4FB5362BC9B4D34BC425E01209EBFFF95A3DA57ACE6E339039426AC.
- Next headset check: open while standing, allow roughly two seconds to settle,
  inspect idle text then walk/rotate and close/reopen. Check text placement on
  the page and transitions before broader chapter acceptance. No recalibration.

### Stable native text candidate rejected by user — 2026-09-11

- User reports text still bobs idle and moves independently when moving.
  Do not mark DF09E6 as fixed/accepted based on the numerical replay.
- Verified installed DLL is DF09E6. Preserved run as
  Builds/journal-text-native-stable-20260911/rejected-run-20260911-162430.log.
  147 samples at frames1685..2561. Native/calibrated matrices become exactly
  constant by sampled frame2165 and remain so through2561; acquisition took
  about480 frames, not the roughly two seconds anticipated. Earlier text is
  still live with full walking motion. Only one journal-button edge recorded.
- After locking, controller-local matrix-derived centre range remains about
  .0006 units, but that measure uses a fixed synthetic glyph centre and the
  controller, NOT actual animated glyph vertices and the visible book. It
  cannot establish rigid text-to-book attachment or contradict user report.
- Inspected recorded generic text shader: position is m_screen * vertex
  POSITION. Dynamic glyph POSITION or difference from actual book motion still
  unmeasured. Do not blindly adjust acquisition thresholds or replace shared
  controller parent. Need distinguish pre-lock delay from post-lock visual
  motion, with direct visual/geometry evidence.
- No rendering changes or redeployment this turn. Analysis script now handles
  shorter captures and reports exact constant-matrix tail. Candidate remains
  installed pending next direction; rollback remains available.

### Submitted geometry diagnostic installed — 2026-09-11

- User confirms text never visually locked, including end of prior run, and
  authorized inspecting actual glyph vertices and book data. Do not attribute
  entire failure to delayed reference acquisition.
- Reused automatic journal frame analysis for at most THREE geometry-only
  frames per process: about6/9/22 seconds after first observed journal-open
  state, separated by >=30 rendered frames. No HOLD, render targets or texture
  captures. VB/IB/CB binary data share a deduplicated timestamped directory
  under `<PRIVATE_DEVELOPMENT_ROOT>/Builds/JournalGeometry-*`.
- H: had only2.5GB free. C: had3.1GB before build/archive, about2.45GB after
  deployment. Capture checks >=2GiB free before EVERY frame and stops if not;
  no files deleted to make room. Captures can hitch and are diagnostic only.
- Important inspection finding: standard FrameAnalysisContext dumps AFTER
  HackerContext returns, by which time private submitted buffers may already
  be restored. Added SnapshotJournalSubmission at private text CB bind and
  inside generic viewmodel draw lambda (after frozen palette binding). These
  record actual bound VB0/VB1/IB and VS CB0/1/3/8 before restoration. Labels
  `journal-submitted-text-*` / `journal-submitted-viewmodel-*`; main log prefix
  `Journal submission`. Limited eye0,32 submissions/frame,16MiB per buffer.
  Ordinary frame log retains draw counts/offsets and shader identity. Geometry
  still needs inspection to establish which draw is the physical book; do not
  assume a mesh identity based solely on index count.
- Retained failed stabilizer and existing matrix/lifecycle diagnostics exactly
  so capture can locate motion despite constant nativePage. No new behavioral
  fix. Scope test reverses scheduler + snapshot helper + two call sites to
  exactly recover DF09E6 HackerContext; VRPose and helper header identical.
  Text/hand/lighter scope/replay guards pass; Release x64 succeeds.
- Metro closed; expected baseline DLL checked; DLL/PDB rollback and previous
  log saved; installed DLL/PDB hashes verified. Archive:
  Builds/journal-submitted-geometry-trace-20260911.
  DLL SHA-256: 8D12E41F9B12EFD8B8E0FD0082EB7717010A22CD1475153F272DBFA37FC2088F.
- User capture sequence: fresh launch, open journal in one level, stand still
  with it open15seconds, then walk with it open15seconds, quit. Brief capture
  stalls expected. Next inspect THREE capture samples and actual submitted
  glyph positions/matrices against book geometry, not a synthetic centre.

### Target clarified; missing capture backend corrected — 2026-09-11

- User now confirms physical journal AND hand bob when walking. That motion
  is acceptable. The task is text rigid relative to the MOVING book, NOT text
  fixed to the controller. This supersedes the prior steady-book assumption.
- Last test fired all three scheduled captures at frames2077,2282,3095 but
  JournalGeometry-20260911-163500-253 contains only empty ShaderUsage.txt.
  No actual VB/IB/CB data saved. Snapshot calls logged, but are virtual no-ops
  on release HackerContext. Game d3dx.ini hunting=0 selects this context;
  log confirms `Creating HackerContext - frame analysis log will not be available`.
  Failed to verify this prerequisite before sending user to capture. Do not
  claim those log messages mean geometry was captured.
- With Metro closed, backed up game d3dx.ini to
  Builds/journal-submitted-geometry-trace-20260911/d3dx.before-capture.ini and
  preserved no-backend-run-20260911-1635.log. Changed ONLY hunting=0 to hunting=2
  (hunting soft-disabled, but creates FrameAnalysisContext). Verified normalized
  config otherwise exact. Existing DLL 8D12E4 unchanged; no rendering code edit.
- NEXT RUN: verify Creating FrameAnalysisContext and actual nonempty binary
  capture files. Restore hunting=0 after successful capture/diagnostic work;
  do not leave performance diagnostic config enabled as a release change.
- Need repeat capture because previous run contains no geometry: journal open,
  stand still15seconds, walk15seconds, quit. Preserve native book/hand walking
  animation in eventual fix; remove failed stabilizer once evidence supports
  replacement, not another controller-locked text solution.

### Actual geometry capture analyzed; moving-page target — 2026-09-11

- This run DID create FrameAnalysisContext and binary data. Directory:
  Builds/JournalGeometry-20260911-164152-428. Snapshot frames2009,2039,2069,
  elapsed6.0/22.9/40.9seconds. Large capture stalls disrupted intended idle/walk
  timing, so do not label the three snapshots definite idle/idle/walk phases.
- Restored game hunting=0 with Metro closed; normalized config compared exactly
  to pre-capture backup. DLL remains8D12E4. Main log archived as
  journal-submitted-geometry-trace-20260911/geometry-run-20260911-164152.log.
- SHARE_DEDUPED puts binary payloads in game-directory FrameAnalysisDeduped on
  H:, despite capture directory on C:. Actual remaining H:~2.39GB, C:~2.75GB.
  Keep the .lnk files and payloads; submission-files.json resolves their targets.
- Tools/inspect_journal_submitted_vertices.py checks the actual690-index text
  draw, standard verified24-byte layout (POSITION float3, COLOR4, UV float2).
  All460 submitted vertex positions and UVs match exactly across buffers
  ffac8c8b/87f4d4b2/9a6ad418, at base vertices3945/3894/4073 respectively.
  Text bounds[-1.7222,64.7778,0]..[930.8334,683,0]. No animated glyph POSITION
  in these samples. The earlier vertex-animation hypothesis is unsupported.
- Captured draw81:2919 indices, start369405, base64389; VS79BA9F1ECFAF0CA1.
  Its1124 exclusively-bone3 vertices have book-sized local extents .227 x .023
  x .295. Shader confirms native position scale12/32768, skin cb8, submitted
  instance VB1, then cb1.m_P at floats24..39. Replay uses actual private VB1
  and CB8 snapshots, not restored generic buffers. Other influenced vertices
  exist; do not treat entire2919 mesh as one rigid page or edit its bones.
- Tools/inspect_journal_book_geometry.py maps final text m_screen through
  inverse(actual book projection * instance * bone3). Text centre in page-local
  coordinates is approximately(.120336,-.002701,-.064081); text sample corners
  have Y about-.00264..-.00277, consistent with the page plane. This establishes
  a measured attachment location, not cross-frame proof of rigid tracking.
- Capture filenames/log reset between frames: book draw81 snapshots from prior
  frames were overwritten while different text draw numbers survived. Cannot
  honestly compare book motion across allthree frames from this artifact.
- Diagnosis direction: preserve native moving-book/hand bob; text vertices are
  static, but failed stabilizer anchors text nativePage rather than the actual
  animated book page. Next candidate should use the book's submitted page
  transform and maintain calibrated page-local placement. Needs freshness,
  opening/closing and changed-hand-family guards; no shared weapon-parent edits.
  No rendering implementation or deployment in this analysis turn.

### GPU book-page attachment candidate installed — 2026-09-11

- User approved page-attached candidate. Preserve physical book/hand walking
  bob; remove independent text motion. Not a controller-fixed text plane.
- Removed JournalPageStabilizer integration, 300-sample matrix diagnostics,
  automatic geometry capture and submission snapshots. Existing second-level
  lifecycle trace in VRPose/right-hand route retained unchanged. hunting=0.
- New JournalBookPage.h copies actual submitted book VB1 instance, CB8 bones,
  and CB1 camera entirely on the GPU, at each eye's generic book draw. Matching
  guard:2919 indices, one instance, VS79BA9F1ECFAF0CA1; bound instance stride64,
  buffer sizes validated. No CPU maps/readbacks. Requires matching context AND
  both eyes captured in current frame. Missing/unknown/stale book data uses
  original fully live text path, never prior-level transform or settling lock.
- Explicit eye0/eye1 arguments are essential: G->vrCurrentEye was not reliable
  for filtering the diagnostic twin snapshots. Draw lambda now takes explicit
  journalEye only to capture; existing draw arguments and transforms unchanged.
- Text-only VS follows measured book shader chain: page-local calibrated text,
  bone-base3 at CB8 float4 rows4..6, first THREE instance rows, captured eye's
  CB1.m_P rows6..9. Instance row3 is metadata and is ignored, as in native book
  shader. Vertex input POSITION/COLOR/TEXCOORD and output SV_Position,
  TEXCOORD0/TEXCOORD1 match native text shader. Pixel shader and geometry stay
  unchanged. VS, CB11..13 and VS SRV15 saved/restored around text draw; twin
  binds corresponding eye snapshot. Standard immediate stereo path retained.
- Uncalibrated page-local reference derived from actual frame2069 submission,
  not chosen offsets. Existing GetJournalCalibration and entire calibration
  arithmetic remain unchanged and operate in page space. Verified on-disk
  settings exactly -.073615 .141142 -.058959 .119332 .114026 0 .605206.
  Tools/derive_journal_page_reference.py reproduces sampled placement within
  .00000445 game units; all460 actual glyphs NDC maxdifference1.36e-5.
- Tools/test_journal_book_gpu.cpp executes REAL shader on D3D11 WARP using
  native24-byte text layout, animated bones, both eye projections and nonzero
  VB offset. Tests snapshots survive later native buffer overwrite, rejects
  stale/incomplete frames, ignores instance metadata row, restores VS/CB/SRV.
  Passes. Tools/test_journal_book_page_replay.py tests all460 captured glyphs
  and page-local invariance through33 simulated page motions. Not headset proof.
- Tools/test_journal_book_page_scope.py reverses only marked page attachment,
  signature/call updates and explicit eye arguments, and accounts explicitly
  for removal of failed lock/diagnostics. Exactly recovers prior rendering/input
  source outside those regions; VRPose unchanged. Accepted journal pulse/hand
  routing C++ tests, hand/lighter replay, and eight focused regression guards
  pass. Release x64 builds. Retired lock/capture-specific tests are historical;
  active guards are new GPU/scope/replay tests and accepted-feature guards.
- Runtime bounded status logs: `Journal book capture` first8 eye captures and
  `Journal book page` attached/fallback transitions max24. These are needed to
  distinguish unrecognized book routing from actual visual failure, not to
  declare success from numeric replay.
- Metro closed; expected8D12E4 DLL checked; rollback DLL/PDB and unchanged
  config/calibration copied; new DLL/PDB deployed and hashes verified. Archive:
  Builds/journal-book-page-attached-20260911.
  DLL SHA-256: EA027537D37C485858C863B3E694E849BE64C7B02D1AC7DB145A68A71D047EE2.
- Awaiting headset acceptance: text should move with page immediately, including
  walking bob, rotations and opening/closing. Verify other hand sections;
  different book shader/mesh identity safely falls back and is not yet proven
  covered. No recalibration requested. Do not describe candidate as fixed until
  user confirms visual attachment.

### Page attachment accepted; second-level wrong-hand failure captured — 2026-09-11

- USER CONFIRMS page-attached text worked. Preserve EA0275 JournalBookPage
  behavior, reference, calibration and GPU binding lifetime. Do not revisit
  text transforms while fixing the separate journal-hand lifecycle bug.
- Existing lifecycle diagnostic caught the reported failure. Preserved log:
  Builds/journal-book-page-attached-20260911/
  second-level-wrong-hand-20260911-170556.log.
- First journal press frame1781: synthetic presented0->1. Native page starts1799,
  GPU book both eyes ready1, attached1. 12132 hand family correctly uses shared
  parent and live grip. Last objective frame2519. No synthetic close press
  appears before transition. Synthetic presented remains1 through hands changing
  to21762 at3796 and through subsequent gameplay (5669).
- Second-level OPEN press frame5915 toggles stale presented1->0. Actual page
  begins5936 while synthetic presented=0. At5937 no-weapon21762 right hand
  therefore routes independent=1, freeze=1: precisely the wrong empty-hand
  route. Loading detector stays-1000 throughout, so loading-only reset tied
  to that existing signal would not address this reproduction.
- This directly establishes stale inferred journal presentation across native
  disappearance/level transition in THIS failure, unlike earlier speculation.
  Also affects pulse choice: stale presented1 selects six-frame close pulse for
  an actual opening. Do not change accepted pulse durations; fix state ownership.
- No runtime/source behavior edits this turn. Next fix should reconcile inferred
  journal state with actual physical book lifecycle, including native closure
  without chord, without treating draw-out as a fresh open. Actual book capture
  can provide presence without relying on delayed/missing objective text or
  the nonfiring loading-panel detector. Preserve accepted text attachment.

### Journal presentation presence candidate — 2026-09-11

- User authorized fixing the captured stale presentation state. Added
  JournalPresentation.h: effective state requires a requested opening AND either
  its opening grace (180 render frames) or physical book presence within120
  render frames. These are conservative gap tolerances, not measured animation
  durations. The accepted108/6 input pulse durations are untouched.
- Exact2919-index/one-instance/VS79BA9F1ECFAF0CA1 book draw publishes frame on
  eye0 independently of GPU-copy success or objective text. IsJournalPresented
  derives effective state from atomic timestamps. NotifyJournalButton reconciles
  to effective state BEFORE publishing the new edge timestamp, then performs
  the existing toggle. This fixes both pulse selection and right-hand routing
  after native book disappearance without our close chord. No loading-panel
  dependency, level counters, mesh-placement changes, or calibration changes.
- Book presence alone cannot reopen a false button latch: draw-out frames after
  explicit closing remain closed. Missing/ignored openings expire. Bounded
  Journal presence edge log (64 presses) records requested/effective/book frame;
  previous lifecycle diagnostics remain available. No heavy captures enabled.
- Scope test reverses only marked presence blocks/include/declaration and the
  one draw notification to recover accepted EA0275 source exactly. Book shader
  header byte-identical. Existing scope helpers explicitly undo new presence
  changes so historical accepted-feature comparisons remain exact.
- Tests passed: actual C++ policy (opening delay, native disappearance, captured
  edge sequence, second opening, close draw-out, missing draws, frame wrap,
  accepted108/6 pulse integration); actual WARP GPU shader execution; all32
  hand routing cases; pulse rearm/menu tests; glyph/page replay; presence/book/
  lifecycle/hand scope guards; lighter and fixed-prologue pose tests; scripted
  arms, visible-hand split, Market, watch time, embedded calibrations, flame and
  release cleanup guards. git diff --check passed; Release x64 built successfully.
- GPU test initially hit Windows loader error because the old test executable
  now sits beside archived mod d3d11.dll. Same newly compiled test passed in
  this candidate's tests subdirectory, where it loads system D3D11 normally.
- Metro stopped; installed EA0275 verified before deployment. Current DLL/PDB
  backed up as d3d11.pre-change.* in Builds/journal-presence-lifecycle-20260911;
  source, config/calibration and candidate binaries archived there. Deployed
  DLL/PDB hashes verified; hunting=0 and journal calibration unchanged.
  DLL SHA256: 5E9BB08907FCF5736487C4A1E2BC5A2E1296A6185D316BEAECB30EBAE52688AC.
  PDB SHA256: 10E1729FA03D413FECB4DCC8AEC3BCC5BDC4CCF91253025618F799CD9A56DD8C.
- Awaiting headset validation, not declared fixed. Reproduce by opening in one
  level first, then first opening in another; also verify closing and accepted
  page attachment. Remaining limitation: a genuinely open book whose identified
  draw is absent for more than120 frames beyond opening grace is treated as
  absent. This policy does not prove coverage of unknown book mesh variants.

### Verified journal/prologue release and diagnostic cleanup — 2026-09-11

- User reports the lifecycle candidate seems good and authorizes final notes,
  diagnostic removal, embedding, commit and merge. Accepted results now include
  prologue hand/watch idle pose, regular/journal lighter placement, short close
  pulse with full-chord release rearm, shared journal grip across hand families,
  text attached to the animated physical page, and native-disappearance state
  reconciliation preventing the second-level first-open wrong hand.
- Removed all temporary diagnostics introduced by this work: journal load/
  button/page/hand lifecycle logs, presence-edge logs, book capture/binding
  status logs, and the old disabled automatic journal frame-analysis function
  plus its call. Retained the real GPU book snapshots and physical-presence
  notification: they implement the accepted fix and are not diagnostics.
  Older unrelated disabled diagnostic systems and historical capture files
  are not changed. Failed stabilizer experiments are not included in release.
- Exact normalized-source comparison against accepted5E9B candidate proved
  HackerContext.cpp and VRPose.cpp differ only by these diagnostic removals
  (including discarding otherwise-unused capture/bind return variables).
  VRPose.h's obsolete capture-only comment now documents presentation ownership.
  Cleanup replacement manifest preserved with the release archive.
- Verified embedding: prologue pose rows, lighter shader-scale anchors, grip
  routing, input pulse and lifecycle policy, page reference and HLSL are compiled
  into the DLL. Existing embedded journal calibration exactly matches the
  accepted on-disk settings. No capture/bin/text file is needed for these new
  features; existing optional calibration overrides remain supported and intact.
- Self-contained Tools/run_journal_tests.cmd builds/runs four actual C++ tests
  including D3D11 WARP, using a separate temp folder to avoid loading archived
  mod DLLs. All passed, along with new source/binary diagnostic-removal guard,
  embedded calibration guard, scripted arms, visible-hand split, Market routing,
  watch time, flame and release-cleanup checks. Release x64 build and diff check
  passed. Earlier archive-dependent diagnostic scope tests describe historical
  builds; the new self-contained release tests are the maintained entry point.
- Metro stopped and expected5E9B candidate verified. Cleanup DLL/PDB deployed,
  hashes checked; settings/calibrations not modified. Rollback and source are in
  Builds/journal-verified-release-20260911; historical captures kept recoverable.
  DLL SHA256: F40E044A35F235E14EBB11F3B34F4913CC6AA3D363005881E0A828463743F46D.
  PDB SHA256: 4026506E04D6FCB4A8D96BD744B473E2DE4BAF8CC306380BDB048E84D176B937.
- Commit scope: accepted implementation, five embedded/helper headers, four
  standalone C++ regression tests, release guard/runner, and this status log.
  Exclude generated binaries, captures, failed experiments and unrelated local
  artifacts. Master is an ancestor at c5b342a; integrate via fast-forward only.
  No remote push requested. No additional pose/calibration retuning performed.

### Pre-rendered video mono-frame activation candidate — 2026-09-11

- Root cause in the checked-in presentation path: the known fullscreen movie
  draw (`VS D2B663AAD70298CE`, `PS 3E0DD02A2D83C903`, unindexed six-vertex
  quad) had no call to `NotifyFullscreenVideoDraw`, so the existing isolated
  mono submission path could never activate. In addition, draw-time `frame_no`
  is incremented by `RunFrameActions` before `SubmitFrameToCompositor`; a
  notification using the draw number would therefore miss the submission.
- Added a narrow exact-pair/shape classifier in `HackerContext::BeforeDraw` and
  publish `frame_no + 1`, the number observed by the upcoming compositor call.
  The existing presentation path then copies the current flat frame once,
  supplies one identical texture handle to both eyes, and disables pose-tagged
  reprojection for that frame. Gameplay stereo, loading screens, pause/menu
  handling, rendering matrices, and all other shader families are unchanged.
- `git diff --check` passed. Release x64 build succeeded with 0 errors and the
  existing project warnings. Candidate and rollback archived in
  `Builds/prerendered-video-mono-frame-fix-20260911`; previous installed DLL
  SHA256 `F40E044A35F235E14EBB11F3B34F4913CC6AA3D363005881E0A828463743F46D`.
  Candidate DLL/PDB deployed while Metro was stopped; installed DLL SHA256
  `801C3B7A45F4BA332055FE14B3F45286F8DA59B9EA8EEC4D81C8423AA57F9294`.
- Awaiting headset validation. This is not declared fixed until the opening
  movie and at least one later pre-rendered scene are confirmed as a single
  fused image and the transition into normal gameplay remains correct.

#### Headset result: rejected and rolled back

- The user confirmed both tested pre-rendered scenes remained doubled. This
  disproves final-submission frame synchronization, shared texture identity,
  and disabling pose-tagged reprojection as sufficient fixes for the defect.
- The candidate source hook was removed immediately. The installed DLL/PDB were
  restored from the archived verified baseline while Metro was stopped; restored
  DLL SHA256 `F40E044A35F235E14EBB11F3B34F4913CC6AA3D363005881E0A828463743F46D`.
- Do not repeat this approach. The next investigation must capture the movie's
  source SRV before it is incorporated into the eye images, suppress that exact
  original draw, and present the captured content on a separately projected
  head-relative screen. That path needs draw/resource evidence before deployment.

### Pre-rendered video OpenVR quad candidate — 2026-09-11

- The rejected candidate's runtime log proves its exact movie classifier and
  mono branch were active from compositor frame 2. Both scenes nevertheless
  stayed doubled. Therefore shared texture identity, synchronized copying and
  removal of pose-tagged reprojection are not sufficient inside OpenVR's
  scene-eye submission path; the failure is not another missed notification.
- Added a materially separate presentation primitive for only the exact movie
  draw. Its completed flat backbuffer is supplied to an OpenVR overlay quad
  attached to the HMD, 3.2 metres wide at a 2.0-metre virtual distance. OpenVR
  projects that single physical plane correctly for both eyes. Opaque black is
  submitted through the normal scene-eye path behind it so the original doubled
  image cannot remain visible around the panel. The overlay is hidden on the
  first non-movie frame and whenever compositor resources are recreated.
- Failure behavior is bounded and reversible: overlay creation, texture, and
  visibility errors are logged; if either overlay or black backing cannot be
  prepared, the code falls back to the unchanged shared scene presentation
  rather than blanking the headset. No Metro shader is replaced or suppressed.
  Gameplay stereo, loading screens, pause/menu paths, brightness processing,
  render targets, and camera/view matrices are unchanged.
- `git diff --check` passed; Release x64 built successfully. Candidate and
  verified rollback are archived in
  `Builds/prerendered-video-openvr-quad-20260911`. Candidate DLL/PDB deployed
  while Metro was stopped. Candidate/installed DLL SHA256:
  `44ED267D950EE5113F98A925C905525FDBEE8956ED5F749BD396E4A24B13BCDC`.
  Verified rollback DLL SHA256:
  `F40E044A35F235E14EBB11F3B34F4913CC6AA3D363005881E0A828463743F46D`.
- Headset validation passed: the user confirmed the pre-rendered presentation is
  correctly fused as one image and described the result as perfect. The OpenVR
  quad is the accepted fix. Keep the exact movie classifier, finite-distance
  head-relative overlay and black scene backing together; the earlier shared
  scene-eye texture approach remains rejected.

### Returned-main-menu character hands — 2026-09-11

- User video showed a black stretched skinned object following the right
  controller only after returning from gameplay to the main menu. Draw tracing
  identified Metro's 12,132-index combined-hand mesh: the ordinary VR route was
  replacing its authored instance/bone ownership with the controller pose after
  the front-end level resumed.
- UI presence, depth, shader identity and player-pointer lifetime were disproved
  as safe gates because they overlap valid scripted/town scenes. A first UI-gated
  suppression removed the artifact but also removed hands in a scripted scene;
  it was immediately rolled back. Do not restore that broad classifier.
- Reverse engineering found Metro's native `menu_mode` setter at executable RVA
  `+0x21ACF0`, game-manager vtable slot `0x128`. A pass-through runtime trace
  observed mode 1 at initial/returned main menu, mode 0 through gameplay and the
  tested scripted scene, and no false main-menu transition during pause. The
  authoritative current byte is on the pointed-to state object:
  `(*(manager + 0x8)) + 0x138`. The reader validates the expected vtable target
  and fails closed on an unsupported executable.
- A candidate that accidentally read `manager + 0x140` instead of dereferencing
  `manager + 0x8` had no effect and was rolled back. The corrected native-state
  gate first suppressed the orphan mesh and was headset-confirmed to remove the
  controller-attached object without affecting gameplay hands or the previously
  missing scripted-scene hands.
- Final accepted behavior does not discard the main-menu character's hands.
  Before any controller instance or bone-palette substitution, mode 1 submits
  the exact hand mesh with Metro's untouched authored transform/palette through
  the established native twin-eye draw path. Modes 0 and 2 retain the existing
  gameplay, scripted and in-game-menu routing.
- Release x64 build succeeded with 0 warnings and 0 errors; `git diff --check`
  passed. Headset validation passed: the user confirmed the character hands look
  good in the main menu while the right-controller artifact remains absent.
  Accepted DLL/PDB and the prior verified hide-hands rollback are archived in
  `Builds/native-main-menu-authored-hands-candidate-20260911`.
  Accepted DLL SHA256:
  `73E7F5CA97A4C7313FEC10932716233A0C4C2552A0D946141DE5DFC4087231C1`.
  Accepted PDB SHA256:
  `98E3C6B7EEA4F5240960BD9643F96FB4746306352261AE3B4E7F07B5FC4E7C6B`.

### Lens-flare depth occlusion and stable-active candidate — 2026-09-12

- RenderDoc identified the exact lens-flare pair (`VS 77840E246BBF6E55`,
  `PS 8281F4D8AE4065B0`) and proved that scene depth was bound while Metro used
  `DepthFunc=Always`. The accepted correction changes only that pair to
  `LessEqual`, retains depth writes off, and applies a measured `-16384`
  rasterizer depth bias. The user confirmed that lights no longer show through
  objects or NPCs and that unobstructed lights remain present. This changes
  flare visibility/occlusion, not lighting or shadow quality.
- Subsequent paired HMD-roll captures proved the remaining pop occurs before
  D3D submission: the exact-pair draw count fell from five to one while the
  submitted vertices, alpha and post-transform positions remained valid.
  Temporary vertex/upload/allocation tracing was removed from the candidate.
- Static engine tracing identified the owning light-entity update at executable
  RVA `+0x330070`. It copies render-node flag bit 6 to flare-instance byte
  `+0x5C` every frame. Native enable/disable paths are separate at `+0x32FF30`
  and `+0x32FFB0`; disable explicitly clears the flare byte. A signature-gated
  hook now restores only a true-to-false flare change made inside the per-frame
  update. A flare already off on entry remains off, preserving scripted light
  switching, while the accepted D3D depth rule still performs real occlusion.
- `git diff --check` passed. Release x64 built with 0 errors and existing
  third-party warnings. Journal C++/GPU tests plus focused scripted hands,
  visible-hand split, Market, menu, watch, laser, flame and release-cleanup
  regressions passed. Metro was closed for deployment; hunting remains 0.
- Candidate and previous installed diagnostic rollback are archived in
  `Builds/lens-flare-stable-active-candidate-20260912`. Candidate/installed DLL
  SHA256: `B7F2E2F3DDF2DE5A2BAD16D9BF958A93E1D567FE668DB07F78C133B1ED28CBFB`.
  Candidate/installed PDB SHA256:
  `8F156D2A49A0CB18322C110E536C67BE0EA84FC58649080A8EA4B67FDA327A33`.
  Previous installed DLL SHA256:
  `4386899A2822A23C3B673A0080629AD73F08BB6DBC9F60256E7AB3F7DC395A22`.
- Awaiting one comfortable headset acceptance check. No exaggerated head tilt
  is required: look naturally at the affected lights, shift position slightly,
  and confirm they remain stable while still disappearing behind an obstruction.

#### Headset result: stable-active hook rejected and rolled back

- The user reports that individual lights still turn on and off with even small
  walking or head movement. This cleanly falsifies restoration of the
  light-entity flare byte after `+0x330070` as sufficient to retain the missing
  submissions. Do not repeat or broaden this state-restoration approach.
- Metro was already closed. The installed DLL/PDB were restored byte-for-byte
  from the archived pre-candidate pair; restored DLL SHA256:
  `4386899A2822A23C3B673A0080629AD73F08BB6DBC9F60256E7AB3F7DC395A22`.
  The ineffective hook and installation call were removed from source. The
  separately accepted exact-pair depth occlusion correction remains intact.
- Next investigation must continue upstream through construction of the actual
  flare submission list. Existing captures already prove the draw count changes
  before D3D while surviving submitted quads remain valid; no further forced
  HMD-roll testing should be requested.

#### Pre-update light visibility-scope candidate

- The rejected candidate's runtime log confirms its exact hook installed. Static
  control flow then explains the unchanged result: immediately after copying
  node bit 6 at `+0x330376`, Metro tests that copied value at `+0x330379` and
  branches around flare construction. Restoring flare byte `+0x5C` only after
  the full update returns is necessarily too late. This is an implementation-
  timing failure, not evidence that the identified node gate is unrelated.
- The corrected hook acts before the same update. Only when flare `+0x5C` is
  already true and node bit 6 is false, it exposes bit 6 for the duration of
  this single light-entity call. Metro then follows its native flare-building
  path. The hook immediately restores only node bit 6 afterward. An explicitly
  disabled flare enters false and never qualifies, so native/scripted light-off
  state remains authoritative. The exact-pair D3D depth fix continues to handle
  actual wall and NPC occlusion.
- Release x64 built with zero warnings/errors. `git diff --check`, journal C++/
  WARP tests, scripted hands, visible-hand split, menu, town ownership, laser and
  release-cleanup regressions pass. Metro was closed for deployment.
- Candidate and rollback are archived in
  `Builds/lens-flare-preupdate-visibility-scope-candidate-20260912`.
  Candidate/installed DLL SHA256:
  `DC4D46686DFA189AE3571A116A546D5491D801F675A85654656D62986BC0403C`.
  Candidate/installed PDB SHA256:
  `8ADE3D9596BA615A4AB0900A4BC732282B769E714CE34DED83585299FF113573`.
  Rollback DLL SHA256:
  `4386899A2822A23C3B673A0080629AD73F08BB6DBC9F60256E7AB3F7DC395A22`.
- Awaiting a normal, comfortable headset check; no exaggerated roll is needed.

#### Headset result: pre-update visibility scope rejected and rolled back

- The user reports no improvement: individual lights still toggle with ordinary
  walking and very small head motion. This falsifies the render-node bit-6 gate
  at `+0x330070` as the final missing-submission gate, even when exposed before
  Metro's internal early branch. The scoped hook and its installer call were
  removed; do not repeat either `+0x330070` intervention.
- Metro was closed and the installed DLL/PDB were restored byte-for-byte from
  the archived rollback. Restored DLL SHA256:
  `4386899A2822A23C3B673A0080629AD73F08BB6DBC9F60256E7AB3F7DC395A22`.
- Current conclusion: the exact flare draws are still being removed by another
  upstream list-construction or culling gate. A robust correction remains
  technically plausible, but is not established and would require deeper
  engine tracing. This lower-priority issue is parked unless the user chooses
  to resume it. The verified through-object/NPC depth correction remains.

#### Clean parked baseline

- Rebuilt after removing both rejected `+0x330070` hooks and all temporary flare
  draw/upload/allocation tracing. The accepted exact-shader depth correction is
  retained. Release x64 built with zero warnings/errors; diff check, menu, laser
  and release-cleanup guards pass. Metro was closed for deployment.
- Clean DLL/PDB and previous diagnostic rollback are archived in
  `Builds/lens-flare-depth-occlusion-clean-baseline-20260912`.
  Clean/installed DLL SHA256:
  `C97967613A68401A1A9258D429A004ADF7BC7E4DE5984DC6C40BFF50550C1961`.
  Clean/installed PDB SHA256:
  `BD2EFF05EF5C2E0531B6E9435104451F2EE971898C8AB006F3A42DE883A97553`.

### Blood and bullet-hole foreground occlusion candidate — 2026-09-12

- Video `Video Project 81.mp4` shows persistent impact marks remaining visible
  while foreground NPCs, objects, pillars and walls cross their screen area.
  Frame-by-frame review distinguishes these from transient blood particles: the
  marks stay anchored to their receiving surface and leak through occluders.
- The existing decal correction supplies Metro's native view to the position-
  buffer comparison while projecting the decal per eye. It also deliberately
  pulls coplanar decals toward the camera (`0.10` pre-divide clip bias plus
  `DepthBias=-65536`) to prevent the previously fixed close-range z-fighting.
- Both identified decal pixel shaders (`9b93189bf55a2727`, solid-red blood;
  `dcde898f44c2ee62`, textured bullet holes) use a one-sided surface test. They
  compute scene depth minus decal depth, then apply `saturate(delta/tolerance)`.
  A foreground surface produces a negative delta, which saturates to zero and
  retains full decal opacity. Once the VR bias wins hardware depth, the shader
  therefore cannot reject a closer NPC or wall. This exactly explains the
  recorded symptom.
- Candidate changes only that normalization to
  `saturate(abs(delta)/tolerance)` in each shader. It reuses Metro's original
  angle-dependent tolerance, alpha fade and discard threshold; there is no new
  distance constant. Coplanar marks remain unchanged, while equal foreground
  and background separations receive equal rejection. The DLL, camera, weapon,
  decal placement and all other passes are unchanged.
- Both HLSL replacements compile successfully as `ps_5_0`; disassembly verifies
  `div_sat` consumes the absolute depth delta before the original discard.
  `Tools/test_decal_symmetric_depth.py` verifies source integration, exact-
  surface retention, symmetry and separated-surface rejection. `git diff
  --check` passes.
- Candidate source/binaries and unchanged clean-baseline DLL/PDB are archived in
  `Builds/decal-symmetric-depth-candidate-20260912`. Metro was closed; the two
  compiled shader binaries were installed with matching hashes:
  `9b93189bf55a2727` SHA256
  `27D6E72E199F68690D1331B6F152DCA5A4317A626B915BC7747E9F1589072C52`;
  `dcde898f44c2ee62` SHA256
  `A5120742BFA73BB9510393737B1029247F17BBEAF5638D2427FB63942D44C45F`.
- Awaiting headset validation. Main regression risk is decal loss/fading at
  silhouettes or on animated receiving surfaces if Metro's stored position
  differs beyond its own material tolerance; verify both through-occluder
  behavior and ordinary visible blood/bullet holes before acceptance.

#### Headset result: symmetric decal-depth test rejected and rolled back

- Video `Video Project 81 (1).mp4` shows the predicted regression materially:
  valid blood and bullet-hole projections are cut into incomplete shapes, and
  the missing portions differ between the two eyes. This rejects the symmetric
  `abs(sceneZ - decalZ)` overlap test. Metro's negative-depth side is part of
  the valid projected decal volume, not merely an unhandled foreground case.
- The two exact shader overrides were removed from the game and preserved in
  the rejected candidate archive. The replacement HLSL and its candidate-only
  source test were removed from the active source tree. The installed DLL was
  unchanged and remains the clean verified lighting baseline, SHA256
  `C97967613A68401A1A9258D429A004ADF7BC7E4DE5984DC6C40BFF50550C1961`.
- The saved native RenderDoc capture confirms that Metro renders this decal
  pass with depth testing enabled, depth writes disabled, `LessEqual`, and a
  D24S8 depth target. Metro's native rasterizer state has zero depth bias. The
  VR path replaces that with `DepthBias=-65536`,
  `SlopeScaledDepthBias=-0.1`, no clamp, plus a `0.10` pre-divide clip-space
  pull. Hardware depth is therefore the correct next investigation boundary:
  determine which accepted VR bias term is pulling decal geometry in front of
  genuine foreground surfaces while retaining the previously validated
  close-range and physical-turn behavior. Do not retry symmetric pixel-shader
  clipping or select another unmeasured threshold.

#### Bounded world-decal surface-offset candidate

- The native capture establishes the correct invariant: the decal pass already
  uses `LessEqual` hardware depth on D24S8, with depth writes disabled and zero
  native raster bias. Metro's original pixel shader must remain one-sided
  because its negative-depth half is valid projected decal volume, as the
  rejected symmetric shader and `Video Project 81 (1).mp4` proved.
- The previous VR path violated that invariant in two distance-dependent ways:
  `DepthBias=-65536` moves the decal by 1/256 of the entire D24 depth range and
  has no physical-distance bound, while the separate `0.10` pre-divide clip
  pull also changes physical separation with distance. Live/immediate twin
  draws received both; batched replay received neither. This accounts for both
  foreground leakage and inconsistent eye/path behavior.
- Candidate replaces both biases with a dynamically compiled vertex shader
  bound only after the existing shared muzzle/decal classifier proves a draw is
  a persistent world decal. It preserves Metro's original pixel shader and its
  original view-position/normal outputs, changing only raster position by
  `0.01` world units along the camera-facing decoded surface normal. `0.01` is
  not a newly tuned threshold: it is Metro's existing minimum angle-dependent
  decal overlap tolerance. The maximum physical pull is therefore fixed at one
  centimetre at every distance. Muzzle flashes retain the original shader.
- Live eye and immediate twin eye inherit the draw-scoped shader swap. Batched
  twin-eye replay independently applies the identical classifier and shader,
  eliminating the previous three-path mismatch. On shader compilation or
  creation failure, the draw safely falls back to Metro's original shader with
  no unbounded depth override.
- Release x64 builds successfully with zero errors and only existing third-
  party warnings. `Tools/test_decal_bounded_depth_offset.py` extracts and
  compiles the embedded shader, verifies its exact original input/output
  signature, checks all stereo routes, and proves no unbounded bias remains in
  executable code. Applicable camera, menu, late-game laser/hand, flame,
  journal-release, prologue, market-hand, and embedded-calibration tests pass;
  the four PowerShell release guards pass. `git diff --check` passes.
- Candidate and rollback are archived in
  `Builds/decal-bounded-surface-offset-candidate-20260912`. Candidate DLL
  SHA256: `749AB2CA0F348957ADF44646B66C17E14829A8DDA7EA9C7A6664D3D1BC4DDA8A`;
  PDB SHA256:
  `2F6DF6C29661A18A9862205438DD3D9A720887309C8A8105689D8F3D47B0C9DD`.
  Verified rollback DLL SHA256:
  `C97967613A68401A1A9258D429A004ADF7BC7E4DE5984DC6C40BFF50550C1961`.
- Awaiting headset validation. Check complete/fused decals, close range and
  physical head turns, genuine foreground occlusion, muzzle flashes, and an
  authored wall overlay before acceptance.
- Metro was closed and the verified clean baseline hash was checked before
  deployment. Installed candidate DLL/PDB hashes match the archive. Both
  rejected pixel-shader override binaries are absent from the game folder.

#### Headset result: bounded decal offset accepted

- The user confirmed the bounded surface-normal offset fixes blood and bullet
  holes leaking through foreground NPCs, objects and walls while restoring
  their complete appearance. Installed DLL SHA256 remains
  `749AB2CA0F348957ADF44646B66C17E14829A8DDA7EA9C7A6664D3D1BC4DDA8A`.
- A single authored wall overlay flickers only at one close, front-on viewing
  position and stops from either side. Other tested areas are unaffected, so
  this isolated content-specific case is parked rather than risking the
  accepted general decal correction.

### Bottom-of-HMD stream-water disappearance investigation — 2026-09-12

- The user clarified that the target is an actual flowing stream in the sewer,
  not wet-floor material or flashlight sheen. Video `Video Project 81 (1).mp4`
  does not visibly capture its bottom-of-headset disappearance. Projective
  alignment between both recorded eyes across 4.5-10.0 seconds finds a median
  lower-view luminance difference of only 1-2 levels, ruling out a large
  one-eye-only omission in the submitted eye textures.
- Four active water vertex shaders write `SV_ClipDistance` from a view-space
  plane. A candidate transformed that plane into each eye's view space and a
  diagnostic variant made the test unconditionally pass. The symptom did not
  change, proving the disappearance is downstream of that clip plane. The
  candidate was removed and archived in
  `Builds/water-clip-plane-eye-space-candidate-20260912`.
- Exported water pixel shaders `63BA4E9A246051B1` and `B66213BEEABC12BB`
  contain Metro's original 3/-1.5 screen-radial local-light mask, which reaches
  zero before the boundary of the wider VR image. A narrowly scoped replacement
  moved the same smooth falloff to the true eye edge, but headset behavior was
  unchanged. The post-run `d3d11_log.txt` then showed that the current graphics
  configuration created `6D7BE4D64280551C` and `C04B62B752A877F2`, not either
  replaced variant. This test was therefore non-participating and does **not**
  disprove the water-local-light or broader forward-water path. The inactive
  overrides were removed and archived in
  `Builds/water-light-edge-mask-candidate-20260912`.
- The installed DLL and configuration were restored byte-for-byte to the
  accepted baseline after both rejected tests. DLL SHA256:
  `749AB2CA0F348957ADF44646B66C17E14829A8DDA7EA9C7A6664D3D1BC4DDA8A`;
  `d3dx.ini` SHA256:
  `D793EC08B4E0AE39307B3A652795FBB1FB50F0CEA2B4A8BC698CBF72749DF05A`.
- Do not send another shader candidate until the disappearing visual layer is
  positively identified. The remaining plausible branches are the surface's
  depth-derived transparency, screen-space reflection, or a separate deferred
  wet-material/specular pass; the current recording does not distinguish them.
- A render-identical diagnostic now writes the two live pixel-shader bytecodes
  once at creation, without a hotkey or capture program. It is archived with the
  exact accepted rollback in
  `Builds/water-live-shader-bytecode-capture-20260912`. Diagnostic/installed DLL
  SHA256: `F69494A37B0A67D446AFC8C944B0CB6EF3B7C7252BC7BF9427C90EED97BE595C`.
  The accepted rollback DLL remains
  `749AB2CA0F348957ADF44646B66C17E14829A8DDA7EA9C7A6664D3D1BC4DDA8A`.

#### Live stream-water eye-depth-space candidate

- The passive capture succeeded on the next ordinary run and wrote both exact
  shaders: `6D7BE4D64280551C` (5,632-byte DXBC) and `C04B62B752A877F2`
  (6,912-byte DXBC). The diagnostic DLL was immediately removed and the exact
  accepted DLL restored before analysis. Captures and rollback are preserved in
  `Builds/water-live-shader-bytecode-capture-20260912`.
- Disassembly establishes the failure mechanism in both live variants. Metro
  samples the hardware depth buffer at the current pixel, linearizes it with
  `depth_xform`, then subtracts interpolated `v2.z`. Both shaders use that
  difference twice in final alpha: a broad `water_blend.w` shoreline fade and
  a sharp `*100` contact fade. A zero or negative difference makes the flowing
  surface completely transparent.
- In the original flat renderer both values share one camera space. The VR path
  intentionally retains `m_WV`/`v2` in Metro's native view space for deferred
  lighting consistency, while raster position and the sampled hardware depth
  use the current HMD eye projection. The mismatch grows with physical pitch
  and screen position, directly accounting for water disappearing only near
  the lower headset view. Neither the previously tested clip plane nor local-
  light radial mask participates in this final-alpha failure.
- Scoped replacements reconstruct the water surface depth from the current
  pixel's `SV_Position.z` using the same `depth_xform` as the sampled scene
  depth. This restores the original same-space subtraction per eye. Animation,
  mask discard, normal maps, cubemap reflection, sun/shadow/specular variant,
  local light, fog, blend constants and the two native fade formulas are
  retained. Compiled disassembly differs by the one additional reciprocal
  needed to reconstruct surface depth and declares only the newly consumed
  `SV_Position.z`; resource slots and all other signatures remain compatible.
- `Tools/test_water_eye_depth.py` passes, both replacements compile as `ps_5_0`,
  and `git diff --check` passes. Candidate sources/binaries are archived in
  `Builds/water-eye-depth-space-candidate-20260912` and included in the tester
  package allowlist. Installed shader SHA256 values:
  `6D7BE4D64280551C` =
  `98AD78120088A306D7ACE243DF2391D77A47AA42703BD86B6380D5FD4A8CA2DF`;
  `C04B62B752A877F2` =
  `2C872BA3744AEC94DE75932781A2667B522C04382677616FDF4A05BF00FBEAB1`.
  The DLL remains the accepted baseline hash `749AB2...DDA8A`.

#### Headset result: eye-depth candidate rejected and rolled back

- The user confirmed that the lower-view stream-water disappearance was
  unchanged. Static shader analysis was therefore insufficient to identify the
  runtime cause, and the hypothesis is rejected rather than adjusted further.
- Both replacement shaders were removed from the installed game and from the
  tester-package allowlist. Their source and binaries remain preserved only in
  `Builds/water-eye-depth-space-candidate-20260912` for investigation history.
  The game is back on the accepted DLL/configuration baseline, with no active
  stream-water override. The issue is deliberately backburnered at the user's
  request.

### Clean tester package with enforced Medium quality — 2026-09-12

- The tester's startup-wide chugging is not yet proven to have a single cause,
  but the prior package had two concrete hazards: it preserved an inherited
  Metro quality preset even though the VR routes require Medium, and its
  `d3dx.ini` enabled `calls=1`, which logs every Direct3D API call.
- The installer now changes only `r_quality_level` to `3` (Metro's validated
  Medium preset) before first launch. It records the prior value in the install
  record and the uninstaller restores only that value, leaving unrelated
  `user.cfg` changes intact. If the user deliberately changes quality after
  installation, uninstall preserves that newer choice instead of overwriting
  it. A missing clean-install config is also handled without replacing a full
  personal configuration.
- Disposable-folder integration tests cover an existing config and a missing
  config. They prove installation selects Medium, uninstall restores/removes
  only the installer-owned quality line, and unrelated post-install edits
  survive. `Tools/test_tester_installer.ps1` passes.
- The release configuration disables API-call, input, debug, unbuffered,
  NVAPI-convergence and NVAPI-separation diagnostic logging; shader hunting and
  frame capture remain disabled. The lightweight normal `d3d11_log.txt` and
  actionable headset warning overlay remain available for compatibility
  reports. The accepted `vr_reticle_scale=0.75` was explicitly preserved after
  verification caught its omission from an initial rejected package draft.
- The new package uses the exact headset-accepted bounded-decal runtime rather
  than the older journal release input. Payload DLL SHA256:
  `749AB2CA0F348957ADF44646B66C17E14829A8DDA7EA9C7A6664D3D1BC4DDA8A`.
  The rejected stream-water shaders, PDBs, source archives, capture files and
  diagnostic artifacts are absent from the ZIP.
- Final package:
  `Builds/Metro2033ReduxVR-Tester-20260912.zip`, SHA256
  `7585387EE8C5ED04BD950326D2EF0731FEDD1F59B856505855D5AF857D1E399D`.
  Payload-manifest hash verification, ZIP-content audit, PowerShell syntax
  parsing, disposable install/uninstall tests and `git diff --check` all pass.
  Runtime behavior on the affected tester's hardware remains to be verified.

### Runtime-shaped output, compositor menu, and storefront compatibility — 2026-09-12

- OpenVR now supplies the final per-eye submission shape. On the Quest 3 /
  Virtual Desktop test runtime this is `3160x3360` per eye at 72 Hz; Metro's
  internal widescreen scene remains independently scaled and is converted only
  at submission. The headset receives the runtime-requested shape rather than
  a hard-coded development-headset resolution.
- Fullscreen startup video retains its original widescreen texture on a
  compositor quad. The VR settings UI is a separate fixed-density 2048-square
  head-relative compositor overlay, so its legibility no longer depends on
  Metro's scene resolution or headset aspect ratio. Both changes have passed
  headset validation.
- The accepted runtime is archived at
  `Builds/compositor-vr-menu-candidate-20260912`. DLL SHA256:
  `DDDBDD12E0212F93B29C9957F1F25D80E1CD628C1ABBC52AEDBC7ABCC800E160`.
- A suspected 60 FPS cap was disproved. The engine's live `target_fps` remained
  1000, `r_vsync` was off, the companion Present did not block, and both
  SteamVR and fpsVR showed the application moving across the 13.89 ms deadline
  rather than being limited to 60. A stationary capture held full P0 GPU clocks
  with stable temperature, power headroom and memory while delivered FPS cycled.
  SteamVR's smoothed overlay therefore displayed gradual 69-to-60-to-69 motion.
- Restarting SteamVR materially changed one session from approximately
  17.21 ms CPU / 14.05 ms GPU to 14.69 ms CPU / 8.31 ms GPU. The exact stale
  SteamVR/Virtual Desktop subsystem is not proven, so no mod-side timing change
  was retained. A redundant-copy candidate failed before the restart and was
  rolled back; installed/source baseline is restored.
- Installer discovery is now shared by install and uninstall and supports both
  Steam library manifests and Epic Games Store `.item` manifests. Discovery is
  separate from compatibility: only the manifest's exact Metro 1.0.0.3 SHA256
  is verified because the runtime contains Steam-build-specific RVAs. A
  different Epic executable hash is reported and requires separate validation.
- Package construction is reproducible: it consumes the exact accepted DLL and
  configuration archive plus the previously verified dependency/shader payload,
  rather than copying mutable files from the live game directory.
- `Tools/test_store_discovery.ps1` validates isolated Steam and Epic layouts.
  `Tools/test_tester_installer.ps1` now performs an end-to-end automatic Epic
  install/uninstall in addition to quality ownership and rollback cases.
- Compatibility package:
  `Builds/Metro2033ReduxVR-Compatibility-v3-20260912.zip`, SHA256
  `B2698211F82DCE06CC5099DB4297C78F0A831BB0E1A3668FBE62689BEC91BCC3`.
  It contains 11 manifest-tracked payload files, the exact accepted DLL, and no
  PDB, runtime logs, captures, water candidate, debugger probe, or object files.
  All payload hashes, PowerShell syntax, disposable install/uninstall cases,
  storefront discovery tests, ZIP audit, and `git diff --check` pass.

### Cross-headset performance and gun-range shooting note — 2026-09-12

- With closely matched SteamVR output sizes, Quest 3 performance was comparable
  to the Galaxy XR / Virtual Desktop baseline. PSVR2 remained less favorable at
  its fixed 90 Hz cadence even after lowering its requested output to
  `3188x3252`; the mod's internal 1.5x Metro scene remained `3840x2132`. The
  observed 30/45-style lows are consistent with SteamVR reprojection divisions,
  not evidence that the tester package runs a different rendering path.
- Shooting was reported substantially below the sights throughout the shooting
  range, for both hip fire and gun-near-face aiming, while nearby regular
  gameplay in another level remained correct. The effect applies to every
  tested surface in that range rather than only its targets. Treat this as a
  deferred range-specific camera/script state issue; do not retune global shot
  calibration or change the headset compatibility path without evidence that it
  reproduces in ordinary gameplay or another location.

### Expanded Visibility launch-time toggle candidate — 2026-09-12

- Root cause: the original `expanded_visibility` setting still gated the v26
  native world-list coverage adapter, but the later CPU screen-occlusion,
  object-frustum and cluster-frustum bypasses were installed unconditionally.
  The checkbox therefore could not disable the broad visibility work or provide
  its intended performance/pop-in tradeoff.
- Candidate behavior: the saved setting is latched once during startup. When it
  is off, none of the three executable bypasses are installed and the native
  coverage adapter remains inactive. When it is on, the complete accepted
  expanded-visibility path remains active. A menu change is saved for the next
  launch and displays `RESTART METRO TO APPLY`; executable bytes are never
  rewritten while Metro's render and worker threads are running.
- The Release x64 build completed successfully. The 2,893-case offline coverage
  suite, 23-case mocked adapter suite, VR-menu verification, and `git diff
  --check` pass. This is static/offline evidence only; headset validation of
  both launch modes is still required.
- Pre-change accepted DLL and settings are preserved in
  `Builds/expanded-visibility-toggle-candidate-20260912`. Candidate DLL SHA256:
  `A207B46E9949E23C2C6745C41CCBDF03B393AAB12083E6562846C47AD3037B38`.
  It was deployed only to the active tester install; the separate development
  copy remained unchanged.

### Public tester configuration correction — 2026-09-13

- Root cause: public pre-release `v0.1.0-test.1` packaged the generic
  `ThirdParty/3Dmigoto/Dependencies/d3dx.ini` instead of the release-safe
  runtime configuration. The shipped file had API-call logging, input logging,
  NVAPI convergence/separation logging, shader hunting, and usage dumping
  enabled. `hunting=1` selects `FrameAnalysisContext` and draws 3Dmigoto's
  lime-green `Stereo disabled` line for legacy NVIDIA 3D Vision; that message
  does not describe the mod's OpenVR stereo output. These development settings
  can also add avoidable runtime overhead.
- The tracked configuration now explicitly keeps actionable headset warnings
  enabled while setting `calls`, `input`, `debug`, `unbuffered`, `convergence`,
  `separation`, `hunting`, shader export, and usage-dump settings to their
  release-safe values. `Packaging/Build-Package.ps1` validates every one before
  creating an output directory and rejects a diagnostic-enabled runtime.
- The tester installer now applies the complete Metro renderer combination used
  for validation: `r_quality_level 3` (Medium), `r_dx11_tess 1` (Very High),
  `r_vsync off`, `r_supersample 1` (SSAA Off), and `r_af_level 1` (16x texture
  filtering). Metro exposes SSAA, not SMAA.
- Each previous value is recorded independently. Uninstall restores only a
  setting that still has the installer-applied value; a later user change is
  preserved. Missing configurations, unrelated content, duplicate-key refusal,
  failure rollback, and legacy `qualitySetting` uninstall records remain
  supported.
- Follow-up verification found that the previous installer wrote
  `<METRO_INSTALL>\user.cfg`, while the live game updates
  `%LOCALAPPDATA%\4A Games\Metro 2033\<profile-id>\user.cfg`. On the development
  machine the latter was newer and still held `r_quality_level 1`, proving that
  game-folder enforcement was ineffective. The installer now resolves the
  active AppData profile: Steam's most-recent 64-bit account ID is converted to
  Metro's lowercase hexadecimal profile folder, a sole existing profile is
  accepted for Epic/manual installs, and ambiguous profiles require an explicit
  selection. The absolute config path is stored for safe uninstall.
- PowerShell syntax parsing, release-configuration rejection/acceptance, and
  disposable existing-config, missing-config, user-change, Epic-rejection, and
  clean-Steam-profile install/uninstall cases pass. The runtime DLL and shader
  binaries are unchanged; a corrected public archive has not yet been
  published.
- Subsequent external testing verified that the Epic Games Store version does
  not work with the current native RVA/runtime assumptions. Public test builds
  are therefore Steam-only: the installer rejects a path identified by an Epic
  manifest and rejects every executable whose SHA-256 is not the verified Steam
  1.0.0.3 hash. The former interactive `YES` override was removed so an
  unsupported binary cannot be installed accidentally.

### VR-owned presentation surface — 2026-09-13

- A tester report and a controlled local comparison established that Metro's
  `1920x1080` output mode split pre-rendered video even with the accepted
  runtime, while changing only Metro's mode to `2560x1440` restored it. The
  headset runtime recommendation was not the cause: OpenVR continued to request
  its own independent per-eye dimensions.
- The VR runtime now owns a fixed `2560x1440` windowed presentation backbuffer
  while leaving DXGI `ResizeTarget` at the user's physical companion-window
  mode. Metro's SSAA-Off scene canvas is normalized to its matching verified
  `2560x1421` geometry only when necessary. Exact reference canvases are
  recognized before ratio conversion so an already-correct scene cannot be
  scaled twice.
- The established 1.0x final-backbuffer submission path remains unchanged;
  isolated high-resolution scene submission is still limited to VR scales
  above 1.0. Repeated zero-sized or already-forced buffer resizes retain the
  last genuine physical-mode request.
- The policy and integration guards, OpenVR output-contract guard, compositor
  menu guard, and Release x64 build pass. A headset run after explicitly
  selecting Metro `1920x1080` retained correct video and world presentation;
  the compatibility report recorded the intended `2560x1440` VR source and a
  successful runtime-shaped submission.

### Pull request 2 performance integration candidate — 2026-09-17

- Community pull request 2 (`8be8779`, `b27cc38`) was merged locally into the
  isolated `codex/pr2-performance-integration` branch from public `main` at
  `c74d466`. Nothing from this branch has been pushed or merged into public
  `main` yet.
- The contributor reports a measured improvement from 36.4 to 71.8 fps in the
  same Quest 3/SteamVR scene. The main changes reconstruct Metro-compatible CPU
  occlusion depth instead of using the broad visibility bypasses, fold eligible
  scene geometry into one two-eye draw, remove synchronous viewmodel readback,
  cache stereo shader variants and hot-path lookups, and correct several
  reflection, flare, light-culling, and chapter-select presentation paths.
- Static validation on the integration branch passes: all eight self-contained
  C++ tests, the VR-menu verifier, the native-camera build verifier, and a
  `Release | x64` DirectX11 build. This is not headset or gameplay validation.
- The always-on `vr_compatibility_log.txt` report was extended without enabling
  3Dmigoto call logging or continuous profiling. It now records adapter vendor,
  device, subsystem and memory sizes; D3D feature level and VPRT capability;
  local/non-local video-memory usage and budget at startup and after warm-up;
  active stereo, twin, scene-fold, depth-fold, occlusion-remap and visibility
  policy; raw folded/doubled/shared draw totals and decline reasons after 300
  accepted submissions; and one OpenVR compositor timing snapshot.
- Known contributor-reported risks remain for runtime validation: the left hand
  uses the previous frame's weapon-instance matrix, video-setting recreation can
  leak UI/profiler resources, the light-culling FOV correction can leave faint
  shapes at the lower edge near some lights, and the shader-variant disk cache
  is not pruned between builds. The existing Expanded Visibility menu wording
  also no longer accurately describes the new default policy because the broad
  bypasses now require explicit marker files.
