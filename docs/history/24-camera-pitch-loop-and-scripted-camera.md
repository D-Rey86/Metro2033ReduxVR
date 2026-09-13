# 24 — The camera pitch loop, and what it was breaking

Session of 2026-08-15. Branch `viewmodel-eye-offset-fix`.

One defect turned out to be behind five separate reported symptoms. This note
records the mechanism, the fixes, one approach that was tried and rejected, and
a reverse-engineering attempt that failed — including why, so it is not
repeated blind.

---

## 1. The root cause

The culling follow drives Metro's camera with synthetic mouse input so the
engine's culling frustum tracks the player's head. It then **booked what it
assumed the engine did** into an accumulator (`VRPose.cpp`, follow loop):

```cpp
errPitch = headPitch - sCullInjectedPitch;              // error vs. our BELIEF
sCullInjectedPitch += -dy * kRadPerMouseUnit * kPitchResponse;
```

This is open loop. The engine's actual camera is never consulted, so the loop
converges on its own bookkeeping and reports success while the camera sits
somewhere else. Any time the engine does not move as commanded — its pitch
clamp, a scripted camera taking control, a sensitivity that is not exactly
`kPitchResponse` — the belief and the camera part company **permanently**,
because nothing ever compares them.

Measured live, `gamePitch` (engine camera pitch after cancellation, i.e. the
residue) across one session:

| frames | residue |
|---|---|
| 715–860 | +13.2° |
| 871–992 | −25.1° |
| 1130 → end | **+16.2°**, steady for every remaining frame |

It never returns to zero. Note the excursion coincides with the scripted
opening — a script takes the camera, our input is swallowed, the accumulator
books it anyway.

### What that one defect caused

The engine camera sat ~16° above the player's line of sight, and everything the
*engine* does used that camera:

- **World tilted** on level load (the rendered view inherited the residue).
- **Pop-in along the bottom of the view** — culling frustum aimed above it.
- **Having to crouch to interact** — the interaction ray aimed above it.
- **Gun sitting too high** — the weapon transform aimed above it.
- **Sight zeroing that would not hold.** Every calibration was measured against
  a residue that changes within a session. This explains three attempts once
  giving 10.7°, −1.2° and 25.6°, and the repeated "I calibrated it and it
  drifted again". *The reference is stable now; a zeroing measured today should
  be the last one.*

---

## 2. Fixes

### 2.1 Close the pitch loop on the engine (`VRPose.cpp`, follow loop)

`ReadCameraVec(0x170, …)` already reads the engine camera's forward vector from
process memory at a fixed RVA. Use it:

```cpp
sCullMeasuredPitch = enginePitch;      // measured, not booked
sCullInjectedPitch = enginePitch;
errPitch = headPitch - sCullMeasuredPitch;
```

Two consequences, both wanted:

- The error becomes a **true** error, so the loop drives the engine camera to
  the head and self-corrects after anything moves it.
- The cancellation becomes **exact**. Stick look up/down is disabled in this
  mod, so the player has no pitch input at all — every degree in the engine
  camera is ours by definition, with no booking error left to accumulate.

`GetCullingCancellationPitch()` returns the measured value with **no lag
compensation**. The `kCullLagFrames` history exists because the booked value was
a *prediction* the engine acted on ~2 frames later; a measurement of current
state has nothing to predict, and delaying it would reintroduce the lag the
delay was invented to remove.

> **Yaw is deliberately still open loop.** Pitch was safe to measure outright
> because the body cannot pitch. Yaw is not: the body has a real heading, and a
> script that turns the character has genuinely changed it. Do not "fix" yaw the
> same way — `baseYaw` feeds the shot.

### 2.2 Additive view build (`HackerContext.cpp` ≈ 5980)

`R = deltaT * origR` applies the head delta in Metro's **camera-local** frame.
Correct only while that camera is level; with pitch present, head *yaw* is
applied about a pitched up-axis and the forward vector swings along a tilted
cone. The level-basis reconstruction removed the resulting roll but preserved
the composed forward exactly, so the forward-direction half of the error passed
straight through — and since it varies with head yaw, the world appeared to
shift as the player turned.

Sum the degrees of freedom instead:

```
yaw   = gameYaw   + headYaw      (both about world up)
pitch = gamePitch + headPitch    (clamped to ±85°)
roll  = headRoll                 (headset is the only source of "up")
```

Measured `swim` (angle between composed and additive forward) at
`gamePitch ≈ +16°`:

| headYaw | 0° | 5° | 15° | 39° | 55° | 77° | 102° | 155° |
|---|---|---|---|---|---|---|---|---|
| swim | 0.00° | 0.32° | 1.34° | 4.0° | 7.5° | 12.7° | 19.6° | ~30° |

Zero at head-centre, growing monotonically — the signature of yaw about a
pitched axis. Zero at centre also proves both builds agree exactly there.

`kBodyPitchFromEngine = false`: the body contributes no pitch. Taking pitch from
the head directly also avoids follow-loop latency in the view.

### 2.3 Head translation uses the body heading (`HackerContext.cpp` ≈ 6242)

`GetHeadTranslationDelta()` returns the **raw tracking-space** offset with only
a handedness flip — it is never rotated into a view frame, despite the old
comment claiming otherwise. It was being mapped through `origR^T`, Metro's full
camera rotation *including* the ~16° pitch, which tilted every head movement:
lean 20 cm forward and the camera also rose ~5.5 cm.

Now mapped with yaw only. Measured `transSwim` (correction magnitude) peaked at
**0.10 m** against 0.39 m of head travel — about a quarter of the movement.

### 2.4 Weapon body frame is level (`HackerContext.cpp` ≈ 6406)

`vrWeaponViewCorrection3x4` built its "body frame" from `origR`, inheriting the
same pitch. A body that cannot pitch has a level frame by definition — yaw only.
Barrel and shot now share one reference.

Shot elevation is *absolute controller pitch* (`VRPose.cpp` ≈ 6270) and was
always world-correct; when the gun appeared to stop agreeing with where rounds
landed, it was the **barrel** that had rotated away from them, not the shots.

---

## 3. Scripted camera handover

With the loop closed it genuinely fights a script for the camera. So:

- **Detect** external control by comparing what the camera did against what we
  *predicted*, with tolerance proportional to command size
  (`kResponseTolerance = 0.35f`, `kPitchResponse` is only approximate). One test
  covers both signatures: camera moving when we asked for nothing, and camera
  ignoring what we asked.
- **Stand down** on pitch *and* yaw while latched. Gating only pitch left the
  yaw half pushing through cutscenes — that was felt as hitching.
- **Offset is measured, never accumulated**: `enginePitch - headPitch`, live.
  Accumulating it and fading on a fixed timer gave the view and the camera two
  independent timelines (loop recovers in ~4 frames, fade took 25) and the gap
  was seen as *the gun bouncing into place afterwards*.
- **Glide back** — recovery capped at ~1.5°/frame (`kRecoverStep = 30`).

### Failed first attempt, for the record

The original latch required our own command to be **quiet** for 3 frames. It
could never fire: a script pulls the camera off the head, the loop sees a large
error and commands on the very next frame. The ladder climb went undetected for
exactly this reason.

---

## 4. Rejected: the stall detector — do not re-add as-is

To catch a script that **holds** the camera still (invisible to a motion test),
a stall detector stood down when the error stopped shrinking. It *detected*
those scenes correctly — and **broke** them: standing down on a camera the game
is actively holding left the view pointing somewhere the scene was never
composed for, with the character's hands in shot.

The lesson is about what "the engine is refusing us" means. It does not
distinguish a script that wants the camera **somewhere specific** from one that
merely **will not let us move it**, and those need opposite responses. No
tuning fixes that; the distinction is not present in the signal.

Reverted at the player's explicit preference: *slight hitching over broken
framing.*

---

## 5. Scripted-state flag hunt — NEGATIVE RESULT

Goal: read Metro's own "a script owns the player" state instead of inferring it.
Done in-process (we are already injected) as a differential scan of the module's
writable data — **444 KB at RVA 0xCD3000**, which does contain the camera object
at `0xD271B0`.

**Round 1** (exact dword equality): 10340 → 4 → 0. Survivors were the *high
halves of 64-bit pointers* (`0x00007FF7…` = exe image, `0x000001AD…` = heap) on
a 16-byte stride, plus `0xD13DBC` null-during-play / heap-during-script. Killed
by the filter, not the data: a script allocates a **fresh object each time**, so
exact equality fails while the meaning holds.

**Round 2** (pointer *classification* — null / module / heap / other — plus
hit/miss scoring instead of elimination): best candidates reached ~70 hits /
16 misses of 86 samples. Far above chance, nowhere near proof.

**Round 3** (watch the 12-address shortlist every frame, log transitions only):
**all twelve cleared.**

- `0xD28A88/90/98`, `0xD3E640/658` — changed **once**, null→heap during the
  first scripted scene, never returned. One-time allocations.
- `0xD235E0/F0/600/630` — float **pairs**, e.g. `0xC134DD9F 43577849` =
  (−11.3, 215.5). Coordinates, not pointers; classified "other" only because
  they are not valid addresses. All transitions outside scripted state.
- `0xD23608` — a counter: 1, 2, 3, 4, 5.

Five transitions total occurred during scripted state and **not one reversed**
when the scene ended. A state flag must toggle both ways on the boundary.

### If resuming this

Do **not** rerun the correlation scan as it stands. Its weakness was never the
scoring — it is that the `SCRIPTED` label comes from the stall heuristic, which
also fires at Metro's pitch clamp. Options, in order of expected value:

1. **Ground-truth labels** — a marker the player presses during a scene. Worth
   more than any further refinement of the scan.
2. **Breakpoint the camera-pitch write** and read the branch that skips it while
   a script holds the input. Finds the condition *directly* rather than hunting
   for something correlated with it. Same class of work as the camera-matrix
   probe (`ArmCameraMatrixProbe`) and the fire hook.

Machinery is left in place and disabled: `kHuntEnabled`, `kWatchEnabled`.

---

## 6. State at end of session

**Fixed:** world tilt on level load; bottom-of-view pop-in; crouching to
interact; gun elevation; world swimming on physical turns; head-translation
tilt; scripted scenes framing correctly with a smooth handback; sight-zeroing
reference now stable.

**Known remaining:** slight hitching in held-camera scenes (the lying-down
conversation). Accepted deliberately — see §4.

**Untouched open items:** vertical/elevation shooting error, bullet-spread
decision, watch digits on the watch, left-hand separation (blocked: hands are
one mesh, 12132), chapters menu, intro video doubling, door/merchant icon.

## 6b. Change-by-change ledger, including what did NOT work

Chronological. "Verdict" is the player's observed result, not intent.

| # | Change | Verdict | Effect |
|---|---|---|---|
| 1 | Levelled reference rotation; `kZeroStartupTilt = false` | **partial** | Roll chain became correct end to end (`postRoll` tracks the headset 1:1). Tilt symptom remained — roll was never the whole story. |
| 2 | Additive view build (§2.2) | **fixed** | World no longer swims when physically turning. Measured error removed: up to 30° of forward-direction error at large head yaw. |
| 3 | Head translation via body heading (§2.3) | **partial** | Real but small: corrected up to 0.10 m of camera misplacement against 0.39 m of head travel. Did not resolve the remaining complaint. |
| 4 | `kBodyPitchFromEngine = false` | **fixed the tilt** | "100% fixed the tilt issue." Also **broke** four things by hiding the residue rather than removing it: gun too high, bullets not matching the barrel, pop-in along the bottom of the view, having to crouch to interact. |
| 5 | Level weapon body frame (§2.4) | **no effect** | Gun still sat high. Correct in itself, but not the cause. |
| 6 | Close the pitch loop on the engine (§2.1) | **fixed** | The real fix for #4's fallout. Pop-in, crouch-to-interact and gun height all resolved together, tilt held. |
| 7 | Scripted handover, first attempt (quiet-command latch) | **failed** | Never latched at all — see §3. Ladder climb still framed wrong, and the gun bounced into place afterwards. |
| 8 | Proportional-tolerance detection + measured offset + glide-back | **fixed** | Scripted scenes frame correctly and hand back smoothly. |
| 9 | Yaw stand-down during scripts; `kReleaseFrames` 20 → 50 | **failed, and harmful** | Did not reduce hitching. Slowed re-levelling. **And caused a constant sideways offset in UI, subtitles, reticle and bullets** — yaw is booked, not measured, so pausing the injection loses whatever the camera did meanwhile. Both parts reverted. |
| 10 | Stall detector (§4) | **detected, then broke** | Correctly identified held-camera scenes and ruined their framing (hands in shot). Reverted at the player's preference. |
| 11 | Scripted-state flag hunt, 3 rounds (§5) | **negative** | No flag found. Shortlist cleared. |
| 12 | Average the two eye frusta for the view-locked UI plane | **failed** | Pushed UI too far left and **doubled the pause menu**. Reverted — see §6c. |

## 6c. UNRESOLVED: UI placement regression

Reported: head-locked UI and subtitles sit slightly right; the ammo counter is
barely visible at bottom-right; possibly shifted down. The player states the UI
was **perfect before** the camera-pitch work and that this began with it.

Measured with the `uiplace` diagnostic (canvas centre pushed through the draw's
own matrix):

```
ndc = (+0.2800, +0.0000)   eyeOff = (-0.0302, 0, 0)
projRow0 = (+0.8942, 0, +0.2530, 0)
```

**`+0.28` is very probably CORRECT.** With an off-axis frustum a point straight
ahead in view space does not land at NDC 0, it lands at the lens-centre offset
(`projRow0[2] = 0.2530`). Averaging the frusta to "centre" it is what broke the
pause menu. `ndcY` is exactly 0, so a vertical complaint is a different term.

Also ruled out: `IsLikelyInGameplay()` is nothing but the engine's own write to
`cb_misc_0` (`sUILastFlat`), which none of this session's changes can influence;
the view-locked path demonstrably runs (373 samples in one session).

**No mechanism has been identified.** Two attempted fixes both made it worse.
The methodological failure worth recording: there is no measurement of UI
placement from BEFORE the regression, and a number without a baseline cannot
tell you it is wrong.

Next steps, in order of cost to the player:

1. Build from `91958bd^` and hand over a known-good DLL to swap in — restores
   the UI, loses the tilt/pop-in/interaction/gun fixes. Needs no test run.
2. Static diff of this session's changes against the paths UI actually reads.
3. Put the pitch work behind separate flags and bisect over ~3 short runs.

Also open: **bullets sit slightly left** after the #9 revert — that revert
corrected the constant offset but appears to have overshot.

## 7. Constants worth knowing

| Constant | Value | Where |
|---|---|---|
| `kResponseTolerance` | 0.35 | script detect, proportional slip |
| `kExternalRate` | 0.0087 rad (~0.5°/frame) | script detect floor |
| `kOnsetFrames` | 2 | latch |
| `kReleaseFrames` | 20 | release (50 was tried: hurt re-levelling, no benefit) |
| `kRecoverStep` | 30 (~1.5°/frame) | post-script glide |
| `kPitchResponse` | 0.54 | Metro applies ~54% of commanded vertical mouse |
| `kMaxFollowPitch` | 1.15 rad (~66°) | held short of the engine's clamp |
