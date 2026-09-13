# Reverse engineering the 4A Engine camera — 2026-08-01

All RVAs are for the verified Metro 2033 Redux build at
`<METRO_INSTALL>\metro.exe` as of this date. The runtime base is
ASLR'd; RVAs are stable.

## Why this replaced value scanning

Many live sessions of content and behavioural memory scanning consistently
found values that *track* the camera but are recomputed copies, not state the
engine reads back. The best behavioural candidate reached |r| = 0.9995 against
the view forward vector and still never confirmed as writable orientation.

Searching for values can only ever find effects. The probe finds the cause:
the engine must write its view matrix into a constant buffer we already
intercept, and `Map` hands us the exact destination address — so a hardware
breakpoint on it traps inside the engine's own code with every register live.

## Tooling

- Python 3.12 + capstone 5.0.7 + pefile, at
  `%LOCALAPPDATA%\Programs\Python\Python312`.
- `Tools/disasm_at.py` — disassembles around an RVA. Sweeps backwards for an
  instruction boundary that lands exactly on the target, because a chosen
  start offset is usually mid-instruction and capstone stops dead at the first
  undecodable byte.
- `Tools/find_refs.py` — finds RIP-relative references to an RVA by sweeping
  every offset and testing `i + 4 + disp32 == target`, then confirming by
  disassembly. Avoids linear-sweeping 10MB of mixed code and data.

### metro.exe is DRM-encrypted on disk

It ships with Steam's DRM stub — note the `.bind` section — and `.text` is
encrypted in the file. Disassembling the file yields ciphertext; the first
attempt decoded the target as a nonsensical `movabs al, [imm64]` followed by
garbage. The decrypted code exists only in the running process, so
`VRPose::DumpGameModuleIfRequested` writes it out from inside. Dumped as a
flat image at virtual addresses, so **file offset == RVA**.

## The probe mechanism

`VRPose::ArmCameraWriteProbe` and friends. Three things had to be right, and
each was wrong first time in a way that looked like a negative result:

1. **Arming must be synchronous and on the writing thread.** The first version
   armed from a worker thread that suspended the render thread, and lost the
   race every time — the engine writes microseconds after `Map` returns.
   Now armed inline via `RaiseException` with a private code, programming
   DR0/DR7 in the handler's context record.

2. **`ContextFlags` must include `CONTEXT_DEBUG_REGISTERS`.** The context a
   vectored handler receives describes only what the trap captured (typically
   `CONTEXT_FULL`), and the kernel restores exactly what `ContextFlags`
   advertises. Without it the debug registers are silently discarded — arming
   "succeeds" and nothing ever fires.

3. **Debug registers are per-thread.** Arming only the render thread found
   nothing; 4A Engine runs ~112 threads and the camera updates elsewhere.
   `ArmWatchpointOnAllThreads` enumerates and programs all of them (suspend,
   set context, resume — with no logging while anything is suspended, since
   the logger takes locks and a suspended lock-holder deadlocks the process).

A self-test arms on a variable we write ourselves before arming for real, so
"arming is broken" and "the game writes from a thread we didn't arm" are
distinguishable in the log rather than both appearing as silence.

## What was found

### Stage 1 — the constant-buffer flush loop, `metro.exe+0x7E6B3C`

A generic routine that uploads dirty constant buffers:

```
mov  eax, [rip+0x53cfe1]   ; dirty mask            -> RVA 0xD23A80
lea  rsi, [rip+0x53d12d]   ; table of ID3D11Buffer* -> RVA 0xD23BD8
lea  r14, [rip+0x53cfde]   ; table of CPU sources   -> RVA 0xD23A90
...
mov  rbx, [r14-8]          ; CPU-side source for this buffer
call [rax+0x70]            ; ID3D11DeviceContext::Map(WRITE_DISCARD)
movaps xmm0, [rbx+rax]     ; copy source -> mapped
movaps [rax-0x10], xmm0
call [rax+0x78]            ; Unmap
```

14 buffers, one dirty bit each. The trapped registers put us at **entry 1** of
both tables — D3D slot `b1`, `cb_main_matrices1`.

### Stage 2 — the view matrix producer, `metro.exe+0x7E469C`

```
movaps xmm4, [rbx + 0x90]        ; camera matrix rows
movaps xmm0, [rbx + 0xa0]
movaps xmm2, [rbx + 0xb0]
movaps xmm0, [rbx + 0xc0]
mov    edx, [rbx + 0x3ec]        ; packed destination
... shufps x8 ...                ; _MM_TRANSPOSE4_PS
mov    ecx, edx
shr    ecx, 0x10                 ; high 16 = constant buffer index
shl    eax, cl
or     [rip+0x53f3ff], eax       ; mark that buffer dirty
movaps [r11 + rax*8 + 0x1d0], xmm0   ; store transposed rows
```

`RBX = RVA 0xD271B0` — a static global, and the same address that appeared in
stage 1's stack scan.

**Confirmed from the dump:** `[obj+0x3EC] = 0x0001000D` packs constant-buffer
index **1** in its high half and destination row **13** in its low half, and
`0xD23C50 + 13*16 = 0xD23D20` is exactly the address that trapped. Every piece
is consistent.

## Established addresses

| What | RVA |
|---|---|
| Constant-buffer dirty mask | `0xD23A80` |
| Table of `ID3D11Buffer*` | `0xD23BD8` |
| Table of CPU-side sources | `0xD23A90` |
| CB staging area base | `0xD23C50` |
| `cb_main_matrices1` staging (`m_V`) | `0xD23D20` |
| **Camera object** | **`0xD271B0`** |
| Camera matrix (transposed into `m_V`) | `0xD27240` (obj+0x90) |
| Second matrix in camera object | `0xD27300` (obj+0x150) |
| Packed (CB index, dest row) | `0xD2759C` (obj+0x3EC) |

Note the camera object is *static*, not heap — one stable address rather than
a pointer chain to re-walk each frame.

Nothing in the image references `0xD23D20` or `0xD271B0+0x90` RIP-relatively
(checked with `find_refs.py`) — they are reached through pointers, which is
why static analysis alone could not find the writer and the breakpoint was
necessary.

## Stage 3 — the camera setter, `metro.exe+0x7E09EC`

A plain setter, `(rcx = camera object, rdx = source matrix)`:

```
movaps xmm0, [rdx]            ; source 4x4
movaps [rcx + 0x90], xmm0     ; -> camera matrix
movaps xmm1, [rdx + 0x10]
movaps [rcx + 0xa0], xmm1
...
```

`RDX` was a stack address, so the caller supplied a temporary. Walking the
return chain, `+0x7E44D0` just forwards its own `rdx` through, and `+0x7EEAE4`
is the real source — a **command-stream interpreter**:

```
mov    rax, rbx              ; rbx = command stream cursor
lea    rdx, [rbp-0x20]       ; stack temp
lea    rcx, [rip+0x5386f9]   ; -> RVA 0xD271B0, the camera object
add    rbx, 0x40             ; advance past a 4x4
movaps xmm0, [rax]           ; copy the matrix OUT of the stream
...
call   [rax+0x18]            ; dispatch to the setter
jmp    0x7ee313              ; back to the interpreter loop
```

**The camera matrix is literal data embedded in a recorded command buffer**,
replayed later. The code that computes it ran earlier, at record time - which
also means frustum culling happened then, not here. That matters: modifying
the matrix at replay time changes what is rendered but NOT what was culled.

## Camera object layout (RVA 0xD271B0), from a live dump

| Offset | Contents |
|---|---|
| `+0x90` | view matrix (world->view), transposed into `m_V` |
| `+0xD0` | projection |
| `+0x150` | inverse view (camera->world); its 3x3 is `+0x90`'s transpose |
| `+0x160` | camera right vector |
| `+0x170` | camera **forward** vector |
| `+0x180` | camera **world position** |
| `+0x190` | inverse projection |
| `+0x3EC` | packed (CB index << 16) \| dest row — `0x0001000D` |

Sample live values: forward `(0.3434, 0.2282, 0.9110)`, position
`(-18.97, -2.06, -137.20)`.

So the camera's orientation and world position are directly readable and
writable at fixed static addresses - exactly what every value scan failed to
find, and reached in three probe cycles once we searched for code instead.

## Next: find the recorder

The remaining question is whether shot direction comes from this camera or
from a separate player/weapon entity. It matters entirely:

- If aim is derived from a player entity, writing that entity's orientation
  gives controller aim while the camera - and therefore culling - continues to
  follow the head. No mismatch, no vanishing props.
- If aim is derived from this camera, view and aim are welded at the source
  and the fire-swing design (Notes/13) becomes the fallback again.

Stage 4 plan: an EXECUTION breakpoint at `+0x7EEABB` (the `movaps xmm0, [rax]`
above) captures `RAX`, the matrix's address inside the command stream. Re-arm
a WRITE watch on that address and the next frame's recorder trips it - and the
recorder holds the player in a register.

## Stage 4 — ambiguous, and why

Phase A captured the command-stream slot (`0x1488BB79F00`) correctly, and
phase B did trap a write to exactly that address — `movaps [rbx-0x50], xmm2`
at `+0x8373FF`, with `RBX-0x50` matching the watched address exactly.

But the surrounding code is a **batch loop**, not a camera update:

```
0x8373AE  movaps [rbx + 0x10], xmm12
0x8373B3  add    rbx, 0x40            ; next 4x4
...
0x8373FB  movaps [rbx - 0x60], xmm1   ; transposed rows out
0x8373FF  movaps [rbx - 0x50], xmm2
0x837403  movaps [rbx - 0x40], xmm10
0x837408  movzx  ecx, word ptr [r11 + 0x800]   ; count
0x837412  jb     0x837240                       ; loop
```

A run of matrix multiplies and transposes over a counted array — instancing or
skinning work, not a single camera. So the command buffer was **reused**: by
the frame we trapped, that slot held different data. The address was right;
the meaning had changed.

This is the reuse risk flagged when stage 4 was designed. Chasing it further
would mean re-validating the slot every frame before trusting a hit, which is
more machinery for a question we can now answer more directly.

## Better tool available now: the camera object as an oracle

Every earlier value scan failed the same way — it could establish
*correlation* but never *causation*, so candidates that tracked the camera
perfectly could never be confirmed as state the engine reads back.

That limitation is gone. The camera object at a static address publishes
ground truth every frame:

- forward vector at `+0x170` — live sample `(0.34344, 0.22817, 0.91104)`
- world position at `+0x180` — live sample `(-18.97, -2.06, -137.20)`

So a scan can now **test causation directly**: find heap memory matching the
camera's forward vector, write a rotated value into a candidate, and check
whether `+0x170` follows on the next frame. If it does, that candidate is
genuinely upstream of the camera. If it doesn't, it is another derived copy
and gets discarded automatically.

That is a definitive test rather than a statistical one, it runs entirely
inside the process, and it needs no judgement from the player — which is what
made every previous scan expensive and inconclusive.

## Player-state hunt — closed, negative

Ran to completion. Final clean run: 118,843 candidates within 40 units of the
camera position, narrowed by **delta matching** (does it move the same way as
the camera, regardless of constant offset) over four rounds against four
different movements. One survivor, `0x1C48BDA4410`, tracked movement exactly
(deltas 0.95, 0.97, 0.49) — but its values sat in a different coordinate
space entirely, and writing it did not move the camera. Downstream, not
upstream.

**Conclusion: the player's authoritative position and orientation are not
reachable as writable float3s in the heap.** Most likely they live inside a
physics body or are recomputed each frame, so any copy we can find is an
output rather than an input.

Four earlier "negative verdicts" in this hunt were my own bugs rather than
findings, and each is worth remembering as a class:

1. **Collected against a cardinal default.** The camera read `(0,0,1)` before
   it was live; a unit-length check passes that happily. Liveness needs
   "not axis-aligned AND changing", not "length ≈ 1".
2. **Locked onto a stack address.** Heap here is `0x000001xx_xxxxxxxx`, stacks
   are `0x000000xx_xxxxxxxx`. Stack frames are full of camera copies that
   evaporate on return.
3. **Stale baseline.** The write test compared against a position from the
   last narrowing round, so ordinary walking read as the camera "following"
   our write — reporting a 33-unit response to a 1-unit nudge.
4. **Un-timestamped sweep.** The background sweep takes seconds and samples
   candidates at different instants, so nothing shared a common baseline when
   it finished. Everything had to be re-read together before the first delta.

The general lesson: a self-verifying test that can only ever say "yes" is not
verification. Sanity bounds (a 1-unit nudge cannot move the camera 33 units)
and repeat confirmation caught what the original design would have reported as
success.

## SOLVED — the shot ray, via coverage differencing

Value scanning kept asking "where is the aim state?" and the answer was
"nowhere writable". The question that worked was **"which code runs when I
shoot that does not run otherwise?"** Behaviour cannot hide the way storage
can.

### The technique

`VRPose::RunFireCodeCoverage`. Mark every executable page `PAGE_GUARD`; the
first instruction fetch from a page faults, we record it, and the guard clears
itself - so it costs one fault per page for a whole sample window, not one per
execution. Sample while not firing, re-arm, sample while firing, diff. Fire
input is injected by us, so both windows are exact and need no timing from the
player.

Result: **2505 pages down to 11**, reproducibly, in one run. Recording the
faulting address (not just the page) gives the exact entry point into each.

Two immediate findings from analysing those pages (`Tools/analyse_pages.py`):

- **The shot code never reads the render camera** - zero RIP-relative
  references to the camera object from any of the 11 pages. Shot direction is
  genuinely independent of the view, which is exactly the separation the whole
  feature needed.
- Three pages are dense with vector math (`0x806000` 77%, `0x4F3000` 48%,
  `0x5DF000` 46%) - the shape of trace/intersection code.

### The fire call: `metro.exe+0x4F3E60`

A thin wrapper that unpacks an object in `RDX` and forwards it:

```
lea   r8,  [rdx + 0x140]
lea   r10, [rdx + 0x150]      ; two 16-byte-aligned vectors
lea   r9,  [rdx + 0x180]
movss xmm2, [rdx + 0x138]
movss xmm3, [rdx + 0x13c]
call  0x4F3EE0
```

An execution breakpoint there, dumping the object beside the camera's own
position and forward, gave:

```
camera position   (31.5287,  1.8408, 13.8065)
camera forward    (-0.1539, -0.1642,  0.9743)

+0x120  (31.5623,  0.0282, 13.8758)   player world position (at the feet)
+0x150  (30.1745,  0.0032, 22.8882)   SHOT TARGET POINT
+0x178   9.2990                        shot distance
```

Camera position walked 9.299 units along camera forward gives
`(30.098, 0.314, 22.867)`; `+0x150` holds `(30.175, 0.003, 22.888)`. Z matches
to 0.02, X to 0.08 - the residual is weapon spread.

### Weapon object layout (pointer in RDX at the fire call)

| Offset | Contents |
|---|---|
| `+0x120` | player world position (feet) |
| `+0x138`, `+0x13c` | scalars passed to the callee (spread/damage) |
| `+0x150` | **shot target point** — write this to steer the bullet |
| `+0x178` | shot distance |

### Confirmed live

Writing `+0x150` at the fire call with a deliberate 30-degree yaw offset put
bullets visibly to the player's right while the camera and weapon stayed put.
Then replaced with the controller's direction — **motion-controlled aiming
works**.

`VRPose::RedirectShot` computes
`target = cameraPos + controllerDirection * distance`, where the controller
direction is the camera's own heading (the player's BODY facing, since the
engine never learns the head moved) offset by how far the controller has
swung from its anchor.

Note the handler keeps the breakpoint armed for every shot by setting the
**Resume Flag** (`EFlags` bit 16) rather than disarming - an execution
breakpoint would otherwise re-trigger on the instruction being resumed to.

### Why this beats everything that came before

- Aim from the controller with **no input injection and no cancellation** -
  all of that machinery (Notes/13) is obsolete.
- The camera is untouched, so **culling matches the rendered view**. The
  vanishing-props problem does not exist in this design.
- **No fire-swing**, so no shot latency.
- Shot origin sits beside the direction in the same object, so bullets can
  leave the **muzzle** rather than the player's head once the weapon model is
  driven from the controller.

Remaining for full 6DOF: drive the weapon's transform (the shared instance
buffer, Notes/10 and the census in Notes/13). That is now purely visual - the
aiming is solved.

## Superseded: stage 3

Armed on the camera matrix at `0xD27240`. Whoever writes it is the camera
update, and it reads from the player — which is where aim ultimately comes
from. It also re-dumps the image on hit, because stage 1's dump caught `.data`
before the camera existed (every matrix was identity, so the layout above was
only confirmable structurally).

## Why this matters beyond aiming

The goal is writing the authoritative orientation directly, so the game's
camera can stay with the player's head while aim comes from the controller.
That removes both problems the injection approach could never solve: the
engine culls to its own camera (so props vanished when aiming off-axis), and
overriding that camera globally collided with shadow passes, scripted
sequences, recoil and cutscenes.

Last Light Redux is the same engine build, so this transfers directly.
