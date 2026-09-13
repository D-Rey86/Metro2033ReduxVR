# Motion-controlled aiming without game memory — 2026-08-01

## The problem this solves

The mod was head-tracked stereo with gamepad aim, which the user correctly
identified as offering nothing over vorpX. Real motion controls needed the
gun to follow the player's hand.

## Why the memory route was abandoned

Repeated content scans and behavioural (unknown-value) scans over many live
sessions consistently found values that *track* the camera but are recomputed
copies, not state the engine reads back. The best behavioural candidate
reached |r| = 0.9995 against the view forward vector and still never confirmed
as a contiguous writable orientation. `EnsureMemoryScannerStarted` is retired
(`kScannerEnabled = false`) rather than deleted.

## What actually works: injection + cancellation

Two facts we already owned, that combine:

1. Synthetic mouse input via `SendInput(MOUSEEVENTF_MOVE)` steers the game's
   aim — measured at 98.5 degrees of response in an earlier test.
2. We render the view ourselves; the game never learns the head moved.

The engine welds aim to the camera and we cannot unweld it. But we know
exactly how far we pushed the camera, so we subtract our own push back out
of the view matrix. The game aims where the controller points; the player
looks where their head points.

- `VRPose::UpdateMotionAiming` — commands the offset.
- `VRPose::GetAimViewCorrection` — reports what to cancel.
- `HackerContext::PatchMappedVRCameraData` — applies
  `V_corrected = Rx(dPitch) * V_game * Ry(dYaw)`, derived from the engine's
  `C = Ry(yaw) * Rx(pitch)` camera convention.

### Design decisions that matter

**The offset is a pure function of the controller's current pose**, not an
integral of per-frame deltas. An integral accumulates calibration error into
permanent drift. Equally, the loop never targets an absolute game yaw — it
commands only *changes* to the offset — so the player's own stick turning is
left completely alone and there is no path to runaway.

**Lag and scale are measured, not assumed.** `UpdateLagScaleEstimate`
regresses commanded input against the game's observed response at each
candidate lag. Live results: **lag = 2 frames, yaw scale ~1.0, pitch scale
0.54** (Metro applies its own vertical sensitivity multiplier). The pitch
figure was a genuine find — the aim loop originally targeted the *commanded*
total, so the gun pitched only half as far as the hand while looking
directionally correct. The target is now pre-divided by the measured scale.

The estimate only affects what is RENDERED, never what is injected, so a bad
fit can wobble the view but cannot feed back into the aim loop.

## Bugs found and fixed

- **Pitch inverted.** `GetControllerAim` converted the controller's aim vector
  to game space with `(-x, -y, z)`, borrowed from the head pose's rotation
  conjugation `S = diag(-1,-1,1)`. That is correct for rotation *matrices* and
  wrong for a bare *direction*: it negates Y, inverting pitch. The correct
  direction mapping is `(x, y, -z)`. Yaw was unaffected because the two differ
  by a constant 180 degrees, which the anchor absorbs — which is exactly why
  only pitch looked wrong.

- **Startup crash.** `InitializeCriticalSection(&sScanLock)` lived inside
  `EnsureMemoryScannerStarted`, so every reader of the shared camera state
  silently depended on the scanner being enabled. Retiring the scanner and
  enabling motion aiming in the same build meant `GetViewAngleSnapshot`
  entered an uninitialised CRITICAL_SECTION on the first Present. Readers also
  never checked the init flag that writers did check, which is why it crashed
  rather than no-opped. Now initialised on first use via `EnsureScanLock`,
  with its lifetime tied to the state it protects.

## Known limitation: culling

The game culls geometry against *its* camera, which we have aimed at the gun
while rendering from the head. Props outside the game's frustum are discarded
before we see them, so they vanish when the hand points far off-axis. The
offset cap (~50 deg yaw / 40 deg pitch) bounds it but cannot remove it. This
is inherent to injection-based aiming, not a tuning problem.

## The better architecture (next)

Stop making one camera do two jobs:

| | Now | Next |
|---|---|---|
| Game camera points at | the gun | **the head** |
| Culling matches the view | no | **yes** |
| Gun model points at | the camera | **the hand, driven directly** |
| Shot direction | camera, continuously | camera, **swung to the gun only when firing** |

The camera stays with the head, so culling is correct. On trigger pull we
swing the camera onto the gun direction for a few frames, inject the click,
and swing back — invisible, because camera rotation is already cancelled out
of the view.

**Blocker:** the gun must point at the hand without the camera moving, which
means driving the weapon's transform directly — the shared per-instance
buffer problem (Notes/09, Notes/10). Writing into that buffer corrupts
hundreds of neighbouring objects; the fix is a private copy plus a CPU-side
shadow of the game's own Map writes.

## Probes now live (passive, no test choreography)

Both answer questions the rewrite depends on, from ordinary play:

1. **Large-delta linearity** (`VRPose probe:` in the log). Fits the same
   regression restricted to large single-frame commands. If the large-command
   scale tracks the overall scale, the game applies big one-frame deltas in
   full and a fire-swing hides in 2-3 frames. If markedly lower, large inputs
   are clamped or smoothed and the swing is too slow to hide. `kMaxStep` was
   raised 50 -> 150 units so fast hand movement populates the bucket (also a
   responsiveness win in its own right).

2. **Instance-buffer census** (`VRPose census:` in the log,
   `HackerContext::IASetVertexBuffers` + `Map`). Read-only and bounded — binds
   nothing, maps nothing, writes nothing, so it cannot reproduce the old
   corruption. Reports the distinct slot1/stride64 buffers, the byte-offset
   ranges used by weapon-xform vs particle vs other layouts, and the Map-type
   distribution. States a verdict outright: whether weapon and prop ranges are
   **disjoint** (a private buffer substituted on weapon binds is safe by
   construction) or **interleaved** (the private copy must shadow the whole
   buffer). The Map-type counts also confirm or refute Notes/11's *deduction*
   that Map is the only legal write path into a DYNAMIC buffer, and a
   WRITE_DISCARD would mean the buffer is reallocated each time, changing how
   the shadow must be maintained.
