# Direct internal-camera HMD follow

## Scope and safety boundary

This investigation replaces only the synthetic-mouse mechanism that makes
Metro's CPU-side camera follow HMD orientation.  It does **not** replace HMD
tracking or `HackerContext::PatchMappedVRCameraData`, which remains the
authoritative rendered-pose path.

Work began from `master` at `dc0248984c521d80864beb526b26272b379d9d6a` on
the isolated branch `codex/direct-camera-hmd-look`.  Before source changes, the
status log and Notes 13, 24, and 33 were read in full.

The installed rollback is still:

- `<METRO_INSTALL>\d3d11.dll`
- SHA-256 `3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985`

A byte-identical rollback copy is archived beside the first diagnostic.  The
installed game DLL was not changed.  The validated Defense watch-timer fix and
the combined `SendLookAxes` path remain untouched.  No Defense camera anchor,
missing-hands work, per-frame file logging, or GPU readback was added.

## Experiment 1: locate the pre-replay camera command producer

### Rejected/insufficient points

- `metro.exe+0x7E09A0` (the earlier probe instruction was at `+0x7E09EC`)
  consumes a completed matrix and derives the replay camera's basis and
  position.  It is still too late: the command stream is already being
  interpreted.
- `metro.exe+0x7E44D0` is the wrapper reached through the replay camera's
  function-table slot `+0x18`; it immediately forwards to `+0x7E09A0` and is
  late for the same reason.
- The old command-slot watch at `+0x8373FF` is not a camera producer.  The
  command-buffer address had been reused by a counted batch matrix loop.
- A direct-call scan for `+0x7EEAC6` has no hits because that address is an
  instruction inside the command interpreter, not a callable source writer.

### Positive static result

The command interpreter begins at `+0x7EE2E0`.  Its jump-table case at
`+0x7EEAA9` is index 20, so the camera command opcode is `0x70000014`.  That
32-bit immediate occurs only once in the mapped `.text` image.

Its producer belongs to the function beginning at `metro.exe+0xD6BE0`:

1. It copies the RDX 4x4 into recorder fields `+0x60..+0x90`.
2. It derives additional camera matrices/vectors in the same recorder object.
3. It writes opcode `0x70000014`.
4. It appends the four matrix rows to the render command stream.

This is a genuine construction/recording point, materially earlier than the
known replay setter.  It is not yet proven to precede every CPU consumer; that
is exactly what the visual/culling/interaction probe must establish.

Twelve static callsites reach `+0xD6BE0`.  `+0x817B4E` is the best initial
main-view candidate because it supplies a dynamic matrix at `[rdi+rsi+0x150]`
and immediately submits the paired projection at `+0x190`.  This selection is
an inference, not a validated fact.

`Tools/find_camera_matrix_vtables.py` is a read-only helper added to make the
function-table/matrix-consumer search repeatable.  Mapped-memory images must be
given to `Tools/disasm_at.py` with `--raw`; omitting it translates RVAs as if
the image were an on-disk PE and produces misleading disassembly.

## Disposable rotation probe

The first diagnostic hooks `+0xD6BE0` only after validating both its prologue
and the relative call at `+0x817B4E`.  The hook is inert by default.  Shift+F10
toggles a five-degree yaw on a private aligned matrix copy only when the hook
was entered from that selected callsite.  It never overwrites Metro's source
matrix, and Shift+F10 immediately restores the pass-through path.  Normal synthetic
mouse follow remains active so this experiment cannot accidentally become a
replacement implementation.

The probe is intentionally unmistakable and bounded in scope.  A headset run
must answer all four questions together:

1. Does the rendered view rotate five degrees?
2. Does Metro's internal camera-forward vector rotate with it?
3. Does the CPU visibility boundary/culling rotate with it?
4. Does interaction direction rotate with it?

Failure of any item rejects this point as the complete replacement mechanism.
Do not disable mouse follow based only on a visible picture change.

## Build and deployment state

Release x64 built with 0 warnings and 0 errors on 2026-09-06.

- Diagnostic: `Builds/direct-camera-construction-probe-20260906/d3d11.dll`
- Diagnostic SHA-256:
  `C0EA1631D86F1251D6EBAB1A4C1E98233BE736E15B0A3A30106638F0BE7B66EC`
- PDB SHA-256:
  `609E188EE116B80C71ED8539121F8904B4EE94F24C0A8A14FD6174BEF0DA55F5`
- Archived installed rollback:
  `Builds/direct-camera-construction-probe-20260906/rollback-installed-3EAB9B24.dll`

The diagnostic was deployed only for the controlled run described below.  The
exact `3EAB9B24...` rollback remains archived and available for immediate
restoration.

## Initial headset result

`Shift+F10` produced a brief, noticeable response, confirming that the toggle
and hook path executed.  It did **not** cause ongoing hitching or a performance
regression.  No obvious rendered-view difference was seen; that is expected to
be masked by the later `PatchMappedVRCameraData` HMD-render path.  The run is
therefore **execution confirmed, authority not yet proven**: culling,
interaction direction, and shooting direction still need explicit checks while
mouse follow remains enabled.

## Probe v2 runtime confirmation

The first deployed build produced no direct-camera install or activation lines
in `d3d11_log.txt`, so the earlier F10 response was correctly attributed to
3DMigoto's config-reload binding.  Added a single bounded startup log for the
`+0xD6BE0` signature gate, rebuilt Release x64 successfully with 0 warnings and
0 errors, and deployed v2.  The v2 DLL hash is
`E1A99A554A6E539C1EE7E6E34DA6AAD7BCD5817109D1C367116034DA35F47236`.
Restart Metro before testing `Shift+F10`; inspect `d3d11_log.txt` for either
the installed message or the one-shot current-byte message.  This is still a
diagnostic deployment, not a mouse-follow replacement.

## Probe v3 signature correction

The v2 runtime log reported live bytes `40 53 48 83 EC` at `+0xD6BE0`.
The signature gate had incorrectly omitted the leading `0x40`, so v2 never
installed the hook and Shift+F10 could not exercise the probe. Corrected the
expected prologue, rebuilt Release x64 with 0 warnings and 0 errors, and
deployed v3. The v3 DLL hash is
`487AF5926A8FE3E1C0926C1E690AD474230791F0E846B4AB1C817B1A5CA4A58A`.
The exact rollback remains archived at
`Builds/direct-camera-construction-probe-v3-20260906/rollback-installed-3EAB9B24.dll`.
Restart Metro before testing Shift+F10; the log should now show the inert
hook installation, then ACTIVE and selected-callsite-hit lines after the
toggle. Mouse follow remains enabled.

## Next decision

## Probe v4 caller report

v3 installed and toggled successfully, but produced no selected-callsite hit;
the inferred `+0x817B4E` caller is therefore not yet proven to execute. Added
a bounded report of up to eight active hook caller return RVAs, rebuilt Release
x64 with 0 warnings and 0 errors, and deployed v4. The v4 DLL hash is
`DC7025665666A253EEE146B51BDCCE6E07D642F3A1D9FB243C76047D2101FDB5`.
This diagnostic does not alter the pass-through camera path except at the
selected caller. Restart Metro, press Shift+F10 once, and provide the resulting
`active caller return RVA` lines so the actual upstream caller can be selected.

## Probe v5 selected caller

The v4 run showed `+0x846F33` repeatedly, with `+0x81EB0A` and `+0x811FE9`
also observed. Disassembly confirms `+0x846F2E` calls `+0xD6BE0` and
`+0x846F33` is the immediate return address. Selected that evidenced caller,
rebuilt Release x64 with 0 warnings and 0 errors, and deployed v5. The v5 DLL
hash is `52E79C0C4E2657448AD55C1103A845140D11FD089ECF3D867193D58FE37BC916`.

The v5 runtime rejected the callsite because `+0x846F33` is the return
address, not the CALL opcode. Corrected validation to use CALL start
`+0x846F2E` (return `+0x846F33`), rebuilt Release x64 with 0 warnings and 0
errors, and deployed v6. The v6 DLL hash is
`664986EE63045715E48E4D903DA78D09340190787336AD8633380CFF29848479`.

## Probe v6 headset result

The v6 runtime log confirms repeated selected-callsite hits while ACTIVE and
the +5-degree probe application. The user observed shadows disappearing when
the probe was enabled. This is the first evidence that the upstream camera
construction affects a camera-dependent engine path such as CPU visibility or
shadow selection, rather than only a late final-matrix replay. Rendered view
rotation remains masked by `PatchMappedVRCameraData`; interaction, shooting,
weapon/laser transforms, and other camera consumers remain to be tested.

## Probe v7 controller toggle

Shift+F10 was awkward for headset testing. Added a temporary rising-edge chord
using both controller grips to toggle the same probe, plus a one-line overlay
indicator. Existing mouse follow and controller actions remain otherwise
unchanged. Release x64 built with 0 warnings and 0 errors; v7 hash is
`6ADE7C8FCCE6F93A6D47CC51F96B61EFC4837416831A064143EE4702298540F7`.

## Probe v7 headset result

The controller chord toggled reliably and the log confirmed repeated selected
`+0x846F33` hits. The attempted overlay is not visible in the VR mod and must
not be relied upon. The user reported interaction, shooting, and general play
appeared correct while toggling the probe; the only clear regression was
shadows disappearing. The existing post-Defense HMD/right-stick problem was
unchanged, as expected because this fixed-offset probe does not replace mouse
follow or its Defense stretching behavior. Treat the upstream point as
promising but not yet a finished replacement. The shadow mismatch must be
rechecked once the fixed offset is replaced by the real HMD delta.

## v8 direct HMD-follow candidate

Replaced the fixed five-degree probe with a reversible direct-HMD experiment
at the validated `+0x846F2E` / return `+0x846F33` construction call. On entry
to direct mode it snapshots the synthetic yaw/pitch already resident in
Metro's persistent camera. For each selected call it recovers the camera world
position, removes that captured synthetic rotation, composes the current HMD
rotation, recomputes translation for the unchanged position, and passes Metro
a private aligned matrix copy.

While direct mode is active, the existing loop still observes camera and
scripted ownership state but emits and books no synthetic mouse or virtual
right-stick follow. Both controller grips toggle direct mode; disabling it
returns to legacy mouse follow on the next frame. The ineffective overlay and
bounded caller report were removed from this play build.

Release x64 built with 0 warnings and 0 errors. Archived/deployed v8 SHA-256:
`10CC8251E7DF39C5E705145C1481C0222B1F5604C320510AF3F09DEEF56C4C09`.
The exact `3EAB9B24...` rollback is included in
`Builds/direct-camera-hmd-follow-v8-20260906`.

## v8 headset result — rejected

With direct mode active, physical HMD turning did not move Metro's internal
camera with the rendered view. Locomotion no longer followed the intended
controller direction and geometry failed to appear. Runtime measurement showed
rendered-vs-engine yaw divergence growing to roughly 50–100 degrees; disabling
direct mode returned it to about one degree.

Therefore `+0x846F2E` is not the authoritative player-camera construction
point. It is a downstream shadow/render submission, consistent with the prior
fixed-offset probe making shadows disappear. v8 is rejected, not a basis for
tuning. The installed game DLL was restored immediately to the exact validated
baseline SHA-256
`3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985`.
The rejected source and binary remain archived on this isolated branch for
reproducibility.

## v9 upstream source-writer watch

Disabled the rejected direct-follow hook and added an automatic, behavior-inert
one-shot write watch at `metro.exe+0xD22F20`, the global view matrix submitted
by the live main-camera recorder paths. Static analysis finds many readers but
no direct writer because Metro reaches the destination through a register. Once
gameplay has been active for 120 frames, v9 arms a four-byte hardware watch,
captures the next writer plus registers and plausible return addresses, then
permanently disarms. It requires no hotkey and adds no periodic logging.

Release x64 built with 0 warnings and 0 errors. Archived/deployed v9 SHA-256:
`C95D26181DDBD3471F820366E04296AD2FA9D1D2C40DC137DE56111E3B1BC179`.
The exact `3EAB9B24...` fallback is archived alongside it in
`Builds/upstream-camera-source-writer-watch-v9-20260906`.

### v9 result and v10 next link

The v9 watch fired at `metro.exe+0xD53A9`, inside a 704-byte aligned state-copy
routine called from `+0x7DF1E4`. Register values and disassembly establish the
matrix chain as `+0xD07730` to `+0xD23270` to `+0xD22F20`; the watched value
was a buffered copy rather than the original constructor output.

Moved the same automatic one-shot watch to the incoming matrix at
`metro.exe+0xD07730`. The rejected direct-follow hook remains disabled and
legacy mouse follow is unchanged. Release x64 built with 0 warnings and 0
errors. Archived/deployed v10 SHA-256:
`7197FEBA143BE9B76D7F00F84ACDE212C83BA7ED32BD445A19FFCFF6476A8712`.
Exact `3EAB9B24...` rollback is archived with v10.

Only after a deliberate headset run should the evidenced caller be kept
or rejected.  If it succeeds on all four signals, the next experiment can feed
the HMD delta through this construction point while leaving mouse follow
available as an immediate fallback.  If it changes only the picture or misses
culling/interaction, use a one-shot caller trace to walk farther upstream; do
not broaden the hook across all twelve camera submissions.

### v10 result and upstream construction probe

The automatic v10 watch fired on the first write to `metro.exe+0xD07730`.
Because x64 data breakpoints report after the store, the captured RIP
`+0xD62A` identifies the store at `+0xD626` inside the rigid view-matrix
constructor beginning at `+0xD560`. The live stack led directly through
`+0x69BB4F` and the call at `+0x1F3960`.

Static reconstruction establishes the complete active path:

1. `+0x2065F0` resolves the live camera object and invokes virtual slot
   `+0x1220`.
2. All three observed camera vtables map that slot to `+0x1F38A0`.
3. `+0x1F38A0` updates the object-owned three-vector block at `object+0x640`,
   obtains two additional camera values through virtual slots `+0xB30` and
   `+0xB40`, and calls `+0x69B900` at `+0x1F3960`.
4. `+0x69B900` copies the position and two orientation vectors into the camera
   manager, normalizes and orthogonalizes the basis, constructs the global
   view matrix at `+0xD07730` through `+0xD560`, and publishes the related
   global camera state at `+0xD076F0`.
5. The earlier trace proved that state is then buffered through
   `+0xD23270` and `+0xD22F20` before command recording/replay.

This path executes from the main frame callback before the later renderer
work, making it materially earlier than rejected `+0x846F2E`. It is still not
proof that CPU culling and interaction consume the changed basis, so the next
experiment is deliberately only a fixed-yaw causation probe. It hooks
`+0x69B900`, validates the unique selected caller/return `+0x1F3960/65`, and
rotates private copies of both input orientation vectors together by 15
degrees. Position, object-owned source state, synthetic mouse follow, and all
cancellation bookkeeping remain untouched. Both VR controller grips toggle
the probe, and disabling it restores the exact original arguments on the next
call. The v10 one-shot writer watch is removed from this play build. Release
x64 built with 0 warnings and 0 errors. Archived/deployed v11 SHA-256:
`2164241616247BD1413E677DCB96351AB8B77B43B96941ED460690FAD066108F`.
The exact `3EAB9B24...` rollback is archived alongside it in
`Builds/upstream-camera-basis-probe-v11-20260906` and was verified again before
deployment.

### v11 superseded; v12 direct HMD A/B candidate

The fixed 15-degree v11 build was archived but deliberately superseded before
headset testing. With the active constructor chain now established, another
artificial-offset test would consume headset time without showing the intended
experience.

v12 uses the same reversible both-grips A/B toggle at the selected
`+0x1F3960 -> +0x69B900` path, but active mode now performs the real experiment:

- captures a roll-free HMD yaw/pitch reference when enabled;
- applies subsequent HMD yaw/pitch directly to private copies of Metro's
  incoming forward/up camera vectors;
- leaves the object-owned source vectors and position untouched, so real body
  turning and authored source-camera changes remain independent inputs;
- stops synthetic mouse/stick camera-follow output while active;
- publishes current, non-delayed cancellation so the existing rendered HMD
  patch does not apply yaw/pitch twice; and
- restores the frozen legacy injection baseline and history when disabled so
  mouse follow resumes without inherited direct-mode turn credit.

Tracked roll remains solely in the proven rendered-pose path, preventing it
from being doubled. This is a full behavior candidate, not a fixed-angle
probe. Release x64 built with 0 warnings and 0 errors. Archived/deployed v12
SHA-256:
`B640A441D0F97D3CEBE4584FD7C0230459A1B5C07CA8629036CCD94E02516B78`.
Exact `3EAB9B24...` rollback is archived alongside it.

### v12 headset result and v13 ownership/locomotion correction

v12 established the essential success condition. With direct mode active,
physical HMD turning retained geometry/CPU visibility and gunshots followed
the intended aim. The runtime comparison was normally within roughly
0.1–0.3 degrees after activation. This proves `+0x69B900` is materially
different from rejected downstream `+0x846F2E`: Metro's internal camera and
its gameplay consumers are moving with the directly supplied basis.

Two defects remained:

1. Left-stick locomotion continued in the unchanged source/body-camera frame,
   not the physically turned HMD frame.
2. Direct composition continued overriding Metro's basis during forced-view
   scripted scenes.

v13 rotates the native left-stick vector by the same activation-relative HMD
yaw while direct mode owns the camera. It also yields the constructor to
Metro's original forward/up vectors whenever the existing native camera,
scripted-viewmodel, or validated player-performance owner is active. Synthetic
follow remains suppressed during that yield so it cannot fight the authored
camera. When ownership releases, the HMD reference is re-anchored on the first
normal camera frame to prevent replaying physical motion accumulated during
the scene. Locomotion correction pauses during authored ownership as well.

Release x64 built with 0 warnings and 0 errors. Archived/deployed v13 SHA-256:
`420209A4FA5C75DC335E8F3F095232526334F7DA7D030C4B2F75966714235097`.
Exact `3EAB9B24...` rollback is archived alongside it.

### v13 headset/video result and v14 complete legacy handoff

v13 fixed physical-turn locomotion, and the user confirmed that geometry and
shooting continued to follow physical HMD yaw. It did not restore forced views
at the main menu, merchants, or scripted scenes. The supplied merchant video
also showed camera/culling-type presentation problems and an interaction-state
failure: a gun-swap prompt would not appear reliably until direct mode was
toggled to legacy mouse follow and then back. After that manual round trip the
prompt worked from every tested physical heading.

The runtime log explains both results. During v13 authored-owner yields, the
hook passed Metro's original `+0x69B900` basis through, but the rest of direct
mode continued publishing direct cancellation and suppressing legacy mouse
follow. Rendered-versus-engine yaw consequently diverged by roughly 100-164
degrees during these intervals. This was an incomplete ownership transfer,
not evidence against the upstream constructor.

v14 makes the ownership transfer complete and deliberately reversible:

- non-gameplay/menu state, pause-menu expectation, authored camera,
  authored viewmodel, or player-performance ownership selects the full
  existing legacy mouse-follow and delayed-cancellation path;
- the original `+0x69B900` vectors pass through untouched during that fallback;
- release is debounced for eight constructor frames, then the refreshed legacy
  cancellation is captured and the HMD reference is re-anchored before direct
  ownership resumes;
- every manual activation of direct mode starts with the same eight-frame
  legacy settling pass, automating the legacy-off/on refresh that recovered the
  missing gun-swap prompt; and
- ordinary direct gameplay remains mouse-free, including the established
  upstream HMD basis, physical-yaw locomotion correction, and exact current
  cancellation.

This does not claim that v14 solves every authored-camera edge case until it is
tested in the headset. The A/B toggle and exact validated rollback remain
available, and no periodic diagnostic or per-frame file logging was added.
Release x64 built with 0 warnings and 0 errors. Archived/deployed v14 SHA-256:
`549375BB8139192155B2FADA990ECE0DA06E2CEA07920E72AA3C1DF14D73B8E9`.
The exact `3EAB9B24...` rollback is verified in
`Builds/upstream-direct-camera-full-legacy-handoff-v14-20260906`.

### v14 headset result — scripted handoff improved, alignment still incomplete

The user confirmed the tested scripted scene works and physical-turn locomotion
continues working. Vendor forced heading still fails. `Video Project 69 (1).mp4`
(209.32 seconds) shows disappearing decals/visibility changes, including the
stairs/sign area, and a failed gun pickup near the end. Physically turning right
slightly and then turning left with the thumbstick restores the pickup icon.
The eight-frame legacy refresh is therefore not a demonstrated interaction fix.

The inspected runtime log shows direct mode resuming after native ownership
release at frame 4451 with a reported release pitch offset of +24.22 degrees.
Subsequent rendered-versus-engine comparisons repeatedly remain about 20-25
degrees apart despite small yaw differences. Later comparisons also show yaw
errors approaching 30 degrees. This contradicts the earlier broad claim that
all upstream gameplay consumers were proven aligned: the earlier successful
conditions do not establish correctness after a scripted handoff.

Code inspection identifies a concrete mathematical inconsistency to address:
`Hooked_UpstreamCameraBasis` composes the reference-relative HMD matrix around
the full incoming source basis, preserving source pitch. By contrast,
`HackerContext::PatchMappedVRCameraData` explicitly builds world-level additive
yaw and HMD pitch, with only the separate authorized scripted pitch contribution.
Its existing comments describe the tilted yaw cone caused by composing around
a pitched source basis. This is a strong explanation for the measured pitch
residue and yaw-dependent errors, but is not yet proof of every decal or pickup
failure's cause. The next camera correction must match the rendered basis
convention upstream, including release easing, and preserve genuine body yaw.

Vendor ownership needs separate evidence: the current menu/authored-owner
selection did not deliver the requested forced heading. Do not assume generic
gameplay state identifies a merchant or extend broad script classification
without a reliable signal. Preserve v14 as the working tested scripted-handoff
checkpoint. No DLL change or deployment was made during this review.

### v15 — world-level direct basis

Replaced camera-local reference-matrix composition with the same additive
convention used by the rendered camera. The constructor now receives yaw equal
to the incoming source yaw plus current-minus-reference HMD yaw, and pitch equal
to absolute HMD pitch plus `GetScriptedPitchOffset()`, capped at the same 85
degrees as rendering. Forward and orthogonal up are reconstructed directly.
This removes the retained source pitch and tilted yaw axis; source position,
source memory, locomotion correction, and v14's authored legacy handoff remain
unchanged. Script release easing contributes equally to engine and rendered
pitch. No new diagnostic output was added.

Release x64 compiled successfully. Numerical basis checks across yaw/pitch
samples confirmed unit-length forward/up, orthogonality, and recovered pitch
within floating-point precision. These checks do not validate engine behavior.
Vendor searches found no established vendor-specific ownership signal; v15 does
not add a speculative classifier or claim to fix vendor forced views. Decals,
pickup interaction, and post-scene alignment need headset confirmation with the
corrected basis before deciding whether a separate consumer hook is necessary.

Candidate SHA-256:
`360DC9CE21272808FBBEE9EBEF1123C99FD160FEC1B936828C2998FE1E5F46CC`.
Archive: `Builds/upstream-direct-camera-level-basis-v15-20260906`, including
the exact `3EAB9B24...` baseline and v14 `549375BB...` rollback copies.

### v15 headset result — better visibility/pickup; sprint exposes movement split

The replacement `Video Project 69.mp4` (82.80 seconds, modified 20:30 on
2026-09-06) and user report establish improved culling and gun pickup, with
remaining visibility loss predominantly near the top of the view. Installed
hash was verified as v15. Archived the runtime log beside v15 as
`headset-video69-upper-culling-sprint.log`. Many logged comparisons are now
0.0-0.3 degrees, unlike v14's sustained 20-25-degree residue. Brief errors of
roughly 5 degrees remain. This supports the basis correction but does not
establish the cause of the remaining visibility failures. Inspect earlier
visibility/portal camera state, projection coverage, and timing; do not assume
another fixed pitch correction or wider FOV is warranted (110-degree CPU
vertical FOV is already configured).

New user observation: sprint fails when physically turned away from the
original heading. The left-stick correction rotates native input into the
unchanged body frame: at a 180-degree physical offset, forward intent becomes
native backward input. This explains why walking direction can look correct
while an engine forward-only sprint gate may reject it. The native sprint
predicate has not yet been traced, so this remains a strongly supported cause,
not a verified disassembly result. Sending another sprint press or forcing
native forward without changing the movement frame would not resolve that
split safely. Next work must locate the movement-heading consumer and preserve
forward/backward input semantics, real body turning, and authored ownership.
Vendor forced heading remains open. No new candidate was deployed for this
review; v15 and both archived rollback DLLs remain intact.

### v16 — preserve native forward intent; rotate world movement

Static disassembly of the archived mapped image identified a real sprint
eligibility predicate at `+0x28E320`. It checks the player's movement-state
forward bit (`player+0x1348`, bit 0), plus existing state, speed and weapon
restrictions. `+0x28CF00` calls it at `+0x28D001/+0x28D031` to accept or clear
the sprint bit (`0x800`). The movement-speed routines consume the configured
`sprint_koef` through `player+0x1B8 -> config+0x9E0`. This confirms why feeding
rotated backward/sideways stick input loses native sprint eligibility.

The local-player branch at `+0x1F3206` compares the player with the active
player pointer before calling `+0x28D110` at `+0x1F32C5`. That function evaluates
movement, constructs a heading from its source matrix (`+0x28D854` onward), and
writes the world-space movement vector at `+0x28D936`. Its fifth argument is
the output vector; the caller then submits it through `+0x28DD70`.

v16 hooks the complete `+0x28D110` call, preserving all thirteen arguments,
including the seventh float timestep. Only selected return `+0x1F32CA` receives
the physical-yaw correction to output X/Z, after native movement processing.
Vertical movement is unchanged. The prior pre-XInput stick rotation is skipped
only when this signature-validated hook installs successfully; failed install
retains v15's correction. Direct mode and authored-ownership gates still apply.
Native sprint restrictions are not bypassed, and no body/source-camera state
is written. Both-grips A/B continues selecting direct versus legacy behavior.

Release x64 compiled successfully. Candidate SHA-256:
`511F23A76E8E952BCB1AAF0189EE71DD3BEC89F96C0B94CC3E565765A5023972`.
Archive: `Builds/upstream-direct-movement-v16-20260906`, with exact baseline
`3EAB9B24...` and v15 `360DC9CE...` rollback copies. Headset validation is still
required for sprint at physical 90/180-degree headings, diagonals, backward
movement, body turns, and scene handoff. The remaining upper-view visibility
and vendor forced-heading issues are unresolved; v16 makes no visibility change.
Read-only disassembly helper: `Tools/inspect_direct_camera_state.py`.

### Architecture correction — v16 workaround rejected; native look-state trace

The user explicitly rejects separate movement/sprint workarounds and requires
behavior equivalent to accepted mouse follow. v16 is not an accepted solution.
Do not build further compensations on it. Preserve the archived experiments,
but pursue the native player look/body state that mouse input updates.

Read-only static investigation identified the following chain in the archived
mapped executable (these are static findings pending live state verification):

- `sens` lives at `+0xCD6C38`, `sens_border` at `+0xCD6C68`, and
  `mouse_curve_num` at `+0xCD6C98`. The sensitivity consumer around `+0x28B345`
  reaches `+0x28BE80` at `+0x28B4C2/+0x28B4FA` with axis actions 5/6 and 7/8.
- `+0x28BE80` checks native control interception, applies inversion/sign rules,
  and updates player accumulators `+0xF4C` and `+0xF48`. Nonzero input updates
  the native input timestamp at `+0xF50`.
- `+0x28BFC0` reads existing look angles `player+0x650/+0x654/+0x658`, processes
  camera ownership/smoothing, and hands the accumulators to the selected camera
  controller at virtual slots `+0x40` and `+0x38` (calls `+0x28C60E/+0x28C648`).
- `+0x28C660` calls native setter `+0x240FD0`, which normalizes angular values
  and writes `player+0x650/+0x654/+0x658`.
- On the unattached-player path, `+0x28C699` calls `+0x240E90` with zero first
  and third angles and the same second angle. This writes body orientation at
  `+0x65C/+0x660/+0x664`, mirrors it to `+0x668/+0x66C/+0x670`, and sets bit 3
  at `+0x712` when appropriate, marking the transform for native rebuilding.
- `+0x23F760` compares these look/body angle sets and uses the native transform
  methods. This is materially different from modifying a final camera basis
  or rotating the resulting movement vector.

Axis labels, signs, ordering against movement/visibility, attachment exceptions,
and actual scripted/vendor control gates require live confirmation. A direct
write to just one angle triplet would omit the native setter's side effects;
do not do that. Also do not credit real thumbstick rotation as HMD injection.
The exact `3EAB9B24...` DLL is restored for the live reference investigation;
v16 remains archived. No speculative native-state patch is deployed.

### Live baseline confirmation of native look/body state

With exact `3EAB9B24...` installed, external PROCESS_VM_READ-only sampling found
the live local player vtable at `+0xA28E70`. At rest, look pitch was 0.055343 rad,
look/body yaw both 1.578117 rad, body pitch zero, and mirrored body angles matched.
During the requested mouse movement, a 30-second bounded capture measured:

- look pitch 0.006076 to 0.616197 rad; body pitch remained exactly zero;
- look and body yaw each ranged from 1.009458 to 2.557922 rad;
- pending pitch accumulator ranged -0.002338 to +0.002062, and pending yaw
  -0.009713 to +0.010736; the captured endpoints retained equal look/body yaw.

The user reported no visible vertical mouse motion. This does not contradict
the captured native pitch change: accepted VR rendering uses HMD pitch while
the mouse-follow loop steers internal pitch. Do not infer a failed mouse pitch
path from the visible headset/mirror view alone.

Live selected camera controller index 0 uses vtable `+0xB44E40`, yaw method
`+0x8FE3A0`, and pitch method `+0x8FE570`. Both call `+0x8FE740`, which applies
the pending axis input with native scaling, angular bounds/soft-limit handling,
then decays the remaining pending input. Its descriptor flags at `+0x1C/+0x1D`
select bound handling; they are NOT by themselves an input-disabled flag.

The mouse handler's separate native eligibility predicate `+0x69B170` scans
camera-manager effectors and returns false when any descriptor flags contain
bit `0x20`. This is a native input gate, preferable to guessing ownership from
an enumerated list of scene descriptors, but vendor coverage still needs proof.
No new game-memory write, injected diagnostic, or DLL deployment occurred in
this capture. The external observer now reports the live controller methods.

### v17 — native angular-state follow prototype (built, not deployed)

The user's report of bad visual culling while manually moving the mouse with
the headset stationary does not invalidate the native-state capture. Accepted
HMD rendering remains separately tracked; this was a state-discovery exercise,
not a baseline culling validation. Native pitch did change despite no visible
vertical mouse look. Do not describe the accepted mouse-follow baseline as broken.

Static matrix-to-angle conversion at `+0x241130` confirms native yaw is
`atan2(forward.x, forward.z)` and native pitch is negative world-forward pitch.
The two selected controller wrappers call shared `+0x8FE740` at `+0x8FE545`
and `+0x8FE712` (return addresses `+0x8FE54A/+0x8FE717`). Its ten-argument
Win64 signature includes reference angle, response factor, bound flags, hard
and soft limit pairs, and a final multiplier. It writes angle/pending input,
preserves native limits, and decays pending input by clamped `1-factor`.

v17 installs signature-checked hooks at `+0x28BFC0` and `+0x8FE740`, starting
inactive. A thread-local local-player scope plus exact wrapper return address
and player accumulator pointer selects each axis. Genuine input is processed
normally once. A second native angular calculation uses private angle/pending
variables for the HMD error, compensating the native response factor; the
native hard/soft limits still determine the applied rotation. Only that second
calculation's actual yaw delta is credited, never the genuine input delta.
The HMD residual is discarded instead of entering the native rate accumulator.
Zero/nonfinite response or negligible correction is skipped. Metro then runs
its unmodified look setter, body setter, transform invalidation, and camera-source
construction. No direct player-angle stores replace those setters.

The `+0x69B900` private-basis hook and `+0x28D110` movement-output hook are not
installed in this build; the pre-XInput movement rotation is also disabled.
No sprint eligibility patch exists. Rendered HMD tracking remains unchanged.
The measured pitch/script observer remains active; native mode no longer takes
the old early return that blindly published requested yaw/pitch as applied.
Native-mode selection suppresses mouse/stick HMD follow even while native
ownership refuses an update. Both-grips disable resumes the existing combined
SendLookAxes loop, preserving actual accumulated rotation credit rather than
restoring the obsolete frozen basis-experiment entry credit.

Activation includes 32 native updates with synthetic follow suppressed to let
previous mouse input settle. This is an initial transition policy, NOT proven
at every native inertia setting or during Defense carry. It must be checked;
do not claim a complete coast-free handoff from the offline model.

#### Live native input ownership check

The user launched the unchanged exact baseline and left it at the main menu.
A bounded external read on PID 40744 (base `0x7FF639B00000`) found input owner
vtable `+0xADD110`, action interception virtual `+0x88 -> +0x29ABD0`, and mouse
ownership predicate `+0xD0 -> +0x29A3E0`. The active camera controller was index
5, still vtable `+0xB44E40`. Look/body yaw differed in this authored menu state,
as expected; do not force them equal outside native ownership.

Disassembly of `+0x29ABD0` shows only actions 9/10 can be consumed; look actions
5..8 return false. No speculative call into this potentially mutating handler
is needed. `+0x29A3E0` is a read-only predicate of `[input+0x190]+2` and
`input+0x137`. v17 checks the exact two live method addresses before accepting
that implementation, then follows the mouse handler's predicate plus `+0xE1`
condition. It also retains the native camera-manager `+0x69B170` gate, mouse
global gates `+0xD07664/+0xD07665`, health, gameplay/pause, and known authored
camera/player ownership guards. Unknown input-owner implementations fail closed.
Vendor behavior itself remains untested; identifying the native gate is not a
claim that the reported merchant issue has been fixed.

#### Build, verification, rollback and remaining work

Release x64 build succeeded. Six offline tests in
`Tools/test_native_state_follow.py` passed: 10,000 randomized unbounded cases
separate real turning from HMD credit; hard-clamped credit; no private residual
coast; zero response; pitch sign/limits; archived executable signatures/calls.
These are mathematical/ABI checks, NOT execution of Metro's soft-limit code or
proof of culling, interaction, performance, ownership, or headset behavior.

Final v17 prototype SHA-256:
`4E6CB5D6A2A69DDDBDA17EAEEBABBCEE0CDFF7C2562464F6B8A8D7E041FE6621`.
Archive `Builds/native-state-follow-v17-20260906` includes DLL, PDB, source and
exact baseline rollback `3EAB9B24...`. Earlier preflight build before live input
verification is retained separately as `Builds/native-state-follow-v17-preflight-20260906`,
hash `385B85D7D7E533FF412F280B0C60AD13973D469A6EB13F865A42AB02FA1C5C06`;
it is superseded and must not be deployed.

Temporary diagnostics: one log confirmation per activation after both axis
corrections reach the native setters. No periodic trace, per-frame file log,
GPU readback, overlay, or new keyboard test. Remove the one-shot diagnostic
from the final validated candidate and archive its evidence. The observer stays
external and bounded with PROCESS_VM_READ only.

Nothing was deployed in this step. The installed DLL was rehashed as exact
`3EAB9B24...`. Do not deploy over a running game. Next: deliberate v17 A/B
headset test, starting from ordinary gameplay and using both grips, with native
look/body/published camera checks where useful. Revalidate the entire requested
matrix, especially physical 90/180-degree forward/sprint, real stick turning,
upper-view culling/decals, vendor forced heading, pickup, scripted handoffs,
and Defense carry/release/watch. All remain unproven. Do not merge or claim
72-FPS-reference smoothness without the user's in-headset comparison.

#### v17 deployment

After the user confirmed Metro was closed, verified no Metro process remained.
Rehashed the archived candidate, exact rollback, and installed baseline before
copying. Deployed the archived Release x64 candidate; installed SHA-256 verified
as `4E6CB5D6A2A69DDDBDA17EAEEBABBCEE0CDFF7C2562464F6B8A8D7E041FE6621`.
The archived exact baseline still hashes to `3EAB9B24...`. Only the game DLL
was replaced; no settings changes. Starts in legacy; both grips toggle native
follow. Awaiting headset validation; branch remains unmerged.

#### v17 headset rejection — scenery moves relative to walls

User supplied `<PRIVATE_VIDEO>/2026-09-06 21-15-02.mp4` (61.70 seconds)
reporting objects moving with the headset. Review of the full contact sheet and
half-second frames from 20–27.5 seconds shows the metal cabinet/barrel geometry
shifting relative to surrounding wall/doorway geometry, not just disappearing.
This fails world stability; v17 is not an accepted replacement.

Archived evidence: `Builds/native-state-v17-video-211502-review` contains the
runtime log and extracted review sheets. The log confirms native-mode activation
and the two-axis hook reaching native setters with look yaw and body yaw both
-2.08182 and zero pending pitch/yaw. There is no logged toggle back to legacy.
It also contains authored-ownership/pass-through transitions and substantial
early engine/render disagreement, followed by mostly small sampled alignment
errors later. Sparse global direction samples do NOT establish that individual
draws or cached object transforms use the same camera state.

Working hypothesis, not a confirmed root cause: native camera/state publication
and rendered camera cancellation may be referring to different snapshots for
some prepared render data. Inspect camera-source update, queued render data,
and `PatchMappedVRCameraData`'s cached original/corrected views together before
changing anything. Do not mask the failure with cabinet/barrel/decals-specific
compensations or disable culling. Also investigate the ownership transitions;
the video and sparse log alone cannot attribute all failures to one mechanism.

No code or installed DLL change made during this review. Installed v17 hash
still verified as `4E6CB5D6...`; exact baseline remains available for rollback.

### v18 — bounded camera-coherence diagnostic, no speculative fix

Static review confirms two world-rendering paths: object CB0 is rebuilt as
`P_eye * V_new * W`; instanced props use camera-baked transforms and camera
CB1 projection `P_eye * (V_new * inverse(V_old))`. They agree only when the
baked transform and correction use the same native view. The v17 log cannot
prove that agreement per draw. This is a hypothesis under test, not a confirmed
cause of the observed moving scenery. No per-object compensation was added.

v18 adds a temporary fixed-memory capture of native post-setter state (before
the caller's next camera-source update), raw main V/iV/P, and sampled object
W/WV with the cached native view. It arms once on both-grips native activation,
starts at the first eligible native update, and stops after ten seconds or
16,384 records. Sample counts are bounded per frame; a nonblocking SRW lock
drops busy samples rather than waiting. No camera rotation probe, extra input,
GPU readback, per-frame file log, or changed culling behavior. Once frozen, a
background thread writes one unique binary capture in the game directory.
After capture, additional matrix copies stop. All instrumentation must be
archived and removed before the final fixed play candidate.

`Tools/analyze_native_camera_coherence.py` validates the binary ABI and reports
raw view/inverse consistency, latest native-look vs submitted-camera yaw, and
sampled `WV` vs `V*W` discrepancies. It can reconstruct an inferred view from
invertible object transforms. Shader/pass attribution, queued timing, native
effects, and skipped samples are caveats; GPU-only instance matrices are not
captured, so a clean object sample does not prove instanced props are correct.

Release x64 built successfully (existing C4250 warning). Six native-follow
offline tests still pass, plus affine reconstruction and capture ABI checks.
No runtime capture result or fix claim yet. Diagnostic SHA-256:
`D0F953B7F6A6C642868E69428D50B8755165E249B30FA88C1C249AEFA7040ABC`.
Archive `Builds/native-camera-coherence-v18-20260906` includes DLL/PDB/source
and rehashed exact `3EAB9B24...` rollback. Verified game absence, v17 installed
hash and archive hashes before deployment; installed diagnostic hash verified
afterward. Settings unchanged. User should enter ordinary gameplay before
activation and reproduce the moving props for the single ten-second capture;
the assistant reads/analyzes the output. Branch remains unmerged.

#### v18 capture result — first-object camera cache is one update behind

Retrieved and archived `native-camera-v18-38180-29953328.bin` and its runtime
log after the user reported completion. Capture successfully stopped and saved
7,414 records: 674 native samples, 1,348 raw camera samples, 5,392 object samples.
No additional user action was needed to retrieve them.

Of the sampled objects, 659 had `max(abs(WV - cachedNativeV*W)) > 0.01`.
Every such discrepancy was the FIRST sampled object of its frame. Reconstructing
the native view as `WV * inverse(W)` matches a camera submitted later in the SAME
frame to within 0.001 in every one of those 659 cases (worst listed examples
round to zero at six decimals). The first object's cached view matches the
preceding frame's recorded view in all 673 cases with a preceding frame available.
Later sampled objects agree. Worst sampled element difference was 0.901335;
inferred forward differences include roughly 3 degrees. These are matrix-element
differences, NOT proof of 0.9 metres of object motion.

Actual chronology on render thread 12344 is object-map, camera-map, object-map,
camera-map, then later objects. Native look updates occur on thread 37088.
The two captured camera writes per frame have identical raw views and identical
yaw cancellation. Their cancellation also matches the latest preceding native
snapshot. Apart from the activation sample (~0.956 degrees), the largest sampled
native-look/submitted-camera yaw error is ~0.052 degrees; view/inverse products
have identity errors around 0.000002. Thus the capture does NOT support a broad
late native-yaw publication or between-these-two-maps cancellation race.

Confirmed narrower issue: `PatchMappedVRObjectData` uses cached corrected views
before the new main-camera map refreshes them. Inspect/synchronize shared camera
and object-buffer lifetimes, not individual object offsets. However, the capture
does not record intervening draws or GPU-only instance matrices. The first CB0
could be overwritten before a visible draw, so this is not yet proof that this
ordering issue explains the cabinet/barrel regression. Do not ship an assumed
fix or claim all scenery symptoms explained without resolving draw consumption.
Analyzer extended with same/adjacent-frame view matching and object ordinals.
No code fix or additional DLL deployment made during this analysis.

### v19 — resolve the missing draw-consumption evidence

The v18 capture measured writes, not whether the first object version reached
a visible draw. Static inspection cannot settle that runtime question. Do not
repair the renderer based solely on a map-order discrepancy that could be unused
setup data. The assistant acknowledged this missing capture stage to the user.

v19 extends the same bounded temporary diagnostic, leaving v17 rendering/native
follow unchanged. Every tracked object map ends the previous version and resets
its observed-use count, including secondary maps skipped by the scene patch.
Suspect versions (`max(abs(WV-cachedV*W)) > 0.01`) retain raw matrices and buffer
identity. A matching bound buffer at `RestoreEye0IfNeeded` records left-eye draw
preparation with shader hashes, viewport, and tracked camera binding (kind 5).
The next map records lifetime end and observed-use count (kind 6). Hash bits are
decoded directly from the binary, avoiding float/NaN conversion loss. Counts
are capped; the capture is still memory-only on game threads and saves once
in the background. No extra GPU maps/readbacks or renderer mutations.

This is not itself proof of a visible cabinet/barrel draw: audit the recorded
shader pair and the actual draw route. In particular, an absent observed call
cannot exclude a route bypassing `RestoreEye0IfNeeded`. Existing v18 parser
compatibility and six native-follow offline tests passed; Release x64 built.

Diagnostic SHA-256:
`F8D867564D4F65927B77EE4ABB1B1F9BB270FAE8F8BD0D3695B10DCFFAEA154D`.
Archive `Builds/native-camera-draw-use-v19-20260906` includes DLL/PDB/source and
rehashed exact `3EAB9B24...` rollback. Verified Metro absent and installed v18
hash before deployment; installed v19 hash verified afterward. No settings
changes or merge. Needs one repeat of ordinary-gameplay both-grips activation
and ten seconds of head movement. Remove all v18/v19 instrumentation after
the evidence is archived and before producing any final fixed play build.

### v19 result and v20 early-camera synchronization candidate

User repeated the capture in an area where nearly everything except the world
was affected. Archived `native-camera-v19-31244-30717062.bin` and runtime log
under `Builds/native-camera-draw-use-v19-20260906`. Its 12,315 records include
613 significant object/view mismatches, all first-object packets. Every inferred
view matches the later camera of the same frame within 0.001. All 613 suspect
lifetimes reached left-eye draw preparation; none were overwritten without
observed use. Recorded VS `1FB92D15F7533B51`, no PS, viewport 3968x2203.
The archived game shader reads CB0 WVP directly for depth output. Thus the
stale corrected object matrix does feed early depth preparation, not merely
unused setup data. Worst matrix-element difference 2.127343, inferred forward
difference 5.657 degrees. This is not proof that every reported symptom has
the same cause. Apart from activation, native/submitted yaw differs by at most
about 0.046 degrees in this sample, again arguing against a broad yaw race.

v20 synchronizes the shared eye-camera caches before patching an early object
whose raw WV disagrees with cached native V*W. It reconstructs current native
V = WV*inverse(W), P = WVP*inverse(WV), validates finite/invertible matrices,
rigid view and main-scene perspective shape, then uses the existing camera
patch routine on a private complete camera packet for both eye caches. W is
inverted explicitly; the shader-unused iW field is not assumed reliable.
The later real camera packet retains its ordinary processing. This repairs
camera-cache chronology, not individual props or body/movement outputs.
Restricted to selected native-follow mode; legacy combined mouse delivery,
native look/body setters, and the Defense timer fix are unchanged.

All v18/v19 runtime capture code and v17 one-shot axis confirmation diagnostics
are removed; their source/binaries/captures remain archived. No GPU readbacks,
extra GPU maps, periodic file logging or probe rotations added. External
`Tools/test_early_camera_sync.py` passes three tests, including 1,000 randomized
reconstructions, all 613 captured mismatches, and a degenerate matrix check.
Six existing native-follow offline tests also pass. These do not establish
GPU/headset correctness or performance parity; atypical render passes and
remaining world-stability/culling symptoms still require headset testing.

Release x64 built successfully. Candidate SHA-256:
`55C2AFE30AF1DD06C067D8131FE5741CB9C947070BF8191D6D061533DEF590B9`.
Archived DLL/PDB/source and exact verified `3EAB9B24...` fallback under
`Builds/native-early-camera-sync-v20-20260906`. Metro was absent before deployment;
installed hash verified against the archive afterward. No settings changes.
Starts in legacy; both grips toggle native follow. Next test is the same
affected area with ordinary head movement and A/B comparison, not a timed
diagnostic capture. Full original validation checklist remains open. No merge
or comparison claim against the exact 72 FPS reference.

#### v20 headset result — moving objects remain, candidate not accepted

User reports: "Nope, things are still moving." Installed DLL was rehashed as
the expected `55C2AFE3...` v20; runtime log confirms native activation, return
to legacy, and native reactivation. Archived this run as
`Builds/native-early-camera-sync-v20-20260906/failed-headset-test-runtime.log`.
This is not an unregistered toggle or wrong installed candidate. The log does
not instrument v20's reconstruction guards, so it cannot prove that the early
cache refresh executed or corrected the depth draw in this run. The offline
reconstruction tests establish only the math, not those runtime conditions.

Do not equate the v19 early-depth mismatch with a complete diagnosis of moving
props. The separate camera-baked instancing path still lacks evidence connecting
the native view baked into its matrices with the view used for projection
correction at the affected draw. That is the next evidence gap to resolve,
along with confirming v20's actual execution, before another fix candidate.
Static reinspection confirms +1F38A0 still performs source update, native look
update, second source update, then +69B900; no new hook moved based on conjecture.
No new DLL, settings change, merge, or automatic rollback during this assessment.
Exact fallback remains archived. v20 remains installed but is not accepted.

### v21 — capture the camera-baked instance path, not another renderer fix

Static review confirms ordinary CB0 world geometry is rebuilt from W, whereas
matrix-instanced props consume camera-baked vertex data through folded camera
projection. The old in-place shared-instance patch remains disabled: its
Map/Unmap corruption and GPU readback cost are already rejected evidence, not
an avenue to revive. No native heading hook, input policy, or renderer math
is changed for v21. The v20 cache synchronization remains under observation.

Temporary `NativeInstanceCapture.h` records existing CPU vertex-buffer writes
and indexed draw entry state. Eight vertex buffers of at most 2 MiB each can
be mirrored; reads occur before the game's existing Unmap, with no additional
GPU Map, staging copy, readback, or writes to game buffers. Only every fourth
frame and a rotating one-in-sixteen eligible vertex Unmap is copied, excluding
square/absent viewports and capped at 8 MiB copied per sampled frame. Missing
copies are explicitly marked unavailable at draw time, not interpreted as zero
transforms. Resource references are retained for this diagnostic process so
reused pointer addresses cannot masquerade as the same mirrored buffer.

Capture starts on first selected native mode, stops after ten seconds or
16,384 records, and writes one uniquely named binary on a background thread.
Only the first capturing thread is observed; a nonblocking lock excludes
overlap. Storage is bounded (16 MiB records, up to 16 MiB mirrors), and sampling
stops afterward. This is a diagnostic build, not a performance/play candidate.
It must be archived and removed again after analysis.

Record ABI: 32-byte `MVRINS21` header; 1024-byte records (`16I, 8Q, 224f`).
Kinds: 1 raw camera entering the existing patch (including v20 private packets),
2 early raw object plus cache state, 3 matrix-instanced indexed draw entry,
4 successful v20 early-camera refresh. Draw records contain first-instance
matrix, raw bound CB1 snapshot, prepared left-eye CB1 bytes, cached correction,
head rotation, cancellation, viewport, shader hashes, mesh/index identifiers,
instance generation, snapshot ages, and explicit availability flags. Samples
exclude viewmodel, square viewport, and no-pixel-shader draws; up to 48 draw
records per sampled frame. Nonindexed/indirect draws are not covered.

Important limits: draw entry may later be skipped or overridden; inspect its
shader and route before declaring GPU use. A WRITE_NO_OVERWRITE buffer's
unwritten bytes are not assumed initialized; only engine-bound ranges are
analyzed. Mesh identity is not unique object identity, and moving/animated
objects require disambiguation. Lack of a mirror is missing evidence, not proof
the transform is wrong. Raw snapshot age and separate bound/cached matrices
are retained to detect stale state instead of assuming coherence.

`Tools/analyze_native_instance_capture.py` validates ABI, reports coverage and
cache-refresh success, compares C*V_native with bound V_eye, and lists inferred
world-position spans as investigation leads only. Three new ABI/hash/recovery
tests pass, along with six native-follow and three v20 reconstruction tests.
An initial compile caught pointer-valued shader-map keys; corrected to the
actual 64-bit hash values before the successful Release x64 build. No runtime
capture or moving-object fix is claimed yet.

Diagnostic SHA-256:
`1A10FD6E1E8C9E49EFAE7EF2363E1C717BED311AC15244A25DE197F35B9A4F87`.
Archive `Builds/native-instance-coherence-v21-20260906` contains DLL/PDB/source
and exact rehashed `3EAB9B24...` fallback; v20 remains separately preserved.
Deployed after checking Metro absent and expected v20 installed hash; verified
installed v21 hash afterward. Settings untouched, branch unmerged. User test:
enter affected area in legacy, stand still, press both grips, turn head for
10–15 seconds without walking/thumbstick turning, then reply done. Assistant
retrieves and analyzes the capture; no overlay, keyboard, or user log inspection.

#### v21 result — prepared camera/instance evidence does not justify another matrix fix

Retrieved and archived `native-instance-v21-36496-33363140.bin` (12,021,792 bytes)
and `capture-runtime.log`. Installed diagnostic hash verified. Capture completed
with 11,740 records and zero copy/slot/capacity drops: 1,384 raw camera records,
1,384 early objects, 668 cache-refresh successes, and 8,304 indexed draw entries.
Every captured raw bound camera snapshot is from its draw's frame. The sparse
CPU mirror supplied 462 complete first-instance samples, also all same-frame;
the other 7,842 deliberately have no instance bytes, not zero transforms.

v20 refresh demonstrably ran on 668 distinct frames. Across all 8,304 prepared
draw states, C*V_native matches bound V_eye within 0.000002414 maximum element
error. Recovering P_eye from prepared VP*inverse(V_eye), P_eye*C matches the
prepared folded P within 0.000002004. Head yaw spans about -13.83 to +12.93
degrees, so this was not an unmoving-head capture. These checks support
coherence of the sampled prepared matrices, not final GPU consumption.

Read the three actual captured VS disassemblies from the game ShaderCache:
DFFE125AB99A0655 is rigid instancing; 79BA9F1ECFAF0CA1 and 03065006306540D5 also
skin through CB8. All three compute clip position with CB1's folded P after
the instance transform. Complete samples: rigid 129, skinned 311 and 22.
The largest naive mesh-position spans are not automatically drift: one mesh
with a 2.16-unit span resolves into two recurring fixed world transforms.
Grouping rigid samples by mesh and reconstructed world transform rounded to
0.001 gives 24 groups; 21 recur on multiple frames (126/129 samples), with
within-group maximum matrix-element span about 0.000002521. This argues against
a broad stale instance/view mismatch in that sampled rigid path. Rounded
grouping is a descriptive check, not an independent proof of object identity.
Animated/skinned meshes and their CB8 palettes are not settled by these data.

Decision: do not deploy another speculative shared matrix correction. Return
to native mouse-versus-hook sequence/side-effect analysis while preserving
these negative results. No proof yet that the angular hook is wrong, or that
all rendering paths are correct. Prepared draw entry can still differ from
actual GPU state after later overrides; indirect/nonindexed and bone data were
not captured. Those limitations must not be erased from the diagnosis.

Static native reinspection: +28BE80 writes pending yaw/pitch and activity time
player+F50. Before normal angular processing, +28C0E3..+28C111 tests F84, F92
bits, F50 against clock D076A4, and nonnull F78 contents. Expiry can branch to
+28C6A0, clearing both pending axes and bypassing look/body setters. Our current
late angular hook does not refresh that activity time. This is a concrete
native-path parity question, NOT an established cause of the moving props;
do not simply stamp the timestamp or force the gate open as a supposed fix.
Re-read shared +8FE740 through its true return +8FEB78 (the initial unwind
entry is only a prologue fragment); its writes observed here target supplied
angle/pending storage, with controller data read by the soft-limit helper.

All v21 runtime instrumentation was removed after archiving; HackerContext.cpp
again matches the v20 source exactly. External analysis/tests remain. Metro
was closed, so restored the EXACT accepted DLL (not a rebuild), verified
`3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985` installed.
No new candidate deployment, settings change, merge, or repeat headset request.
Native investigation remains on the branch; ordinary play is back on baseline.

### Native-path parity follow-up — scope the activity gate before changing it

Read the complete +28B280 mouse handler through +28B562, its +28BE80 action
consumer, normal look processing +28C117..+28C660, and indexed executable
references to F48/F4C/F50/F84/F92. Apart from sensitivity/curve/interception
and wheel dispatch, the inspected mouse handler reaches the pending-angle
accumulators and F50 activity stamp; no additional broad renderer rebuild
notification was identified there. This is a bounded static finding, not a
claim that every caller/indirect consumer in the engine has been exhausted.

The native look gate is precise: F92 bit 1 (value 2) blocks independently.
Otherwise timestamp expiry matters only when F84 is nonzero, F92 bit 0 is set,
and F78 points to a live nonnull owner. Native unsigned (F50+F84) must be strictly
less than clock D076A4; equality does not expire. F92=0 cannot be made to pass
or fail this gate by refreshing F50. Thus treating this as a generic "HMD
activity" flag would be wrong. The timeout is configured through an owner
setup path around +2879B9/+2879CA, and related predicates +208F60/+28CB90 and
the owner update +2899F0 also consume it. The +1F1F6C/+1F1FC1 pending-field
references are serialization-shaped writes, not evidence of a renderer consumer.

Normal +28BFC0 continues through selected camera-controller yaw/pitch wrappers
and the same look/body setters even with zero pending axes (provided its native
eligibility gates pass). No evidence found for "mouse must be present to call
the ordinary setters." +289730 runs before the look update and clears expired
owner state/response ramps; this is another reason not to stamp activity or
move the angular hook without understanding native ownership order.

Extended external PROCESS_VM_READ-only `Tools/observe_native_look_state.py`
with F50/F84/clock, owner-pointee validity, exact reconstructed gate outcome,
response scalars, and the camera-source vectors. No injected code, game writes,
extra input, DLL replacement, or headset test is necessary for this check.
Unknown owner reads remain unknown, not assumed enabled. These are asynchronous
external snapshots, not atomic observations of a particular native instruction.
Pure `native_look_gate.py` and five tests cover all conjuncts, explicit blocking,
deadline equality, unknown owner and native unsigned-add behavior; tests pass.

Next required observation: exact baseline running in the affected gameplay
area, initially with no input needed. Determine whether that ownership path is
even active before considering it relevant to the regression. Metro was closed
at this checkpoint. Accepted DLL remains installed; no new candidate was built
or deployed, and no production C++ source was changed during this follow-up.

#### Live ownership check — timestamp hypothesis not supported in the affected area

User loaded the affected area with exact accepted `3EAB9B24...` installed;
rehashed the DLL before reading PID 37372, image base 0x7FF639B00000. Used only
PROCESS_VM_READ, with no input generation or injected code. Ten-second sample
followed by a compact three-second confirmation consistently reports F92=0,
F78=null, F84=0, and blocked=false (`timeout_disabled`). F50 stays at 1039003
while the native clock advances. Therefore an old activity timestamp does not
block normal look processing in this observed baseline state; do not stamp it
as a proposed general moving-prop fix. This does not settle vendor/scripted
ownership states, nor prove the candidate can never transition into another one.

Local player 0x1EC20A65090 has vtable +A28E70, selected controller index 0,
vtable +B44E40 and the expected +8FE570/+8FE3A0 pitch/yaw methods. Look pitch
-0.03092432, look/body/mirrored yaw -0.81525135; pending axes zero. Camera-source
forward is (-0.72754991, 0.03091939, 0.68535757), consistent with look angles.
The shared published-forward global usually matches but intermittently reads
(0,0,~1) in the compact confirmation, even on the accepted baseline. External
reads are not tied to a render pass, so do not call that transient a new bug
or use this mutable global as unconditional proof of gameplay orientation.

`Builds/native-owner-readonly-20260906/observed-samples.json` preserves 50 parsed
samples retained from the first tool output; that output was bounded, so this
is explicitly a retained subset, not a full ten-second trace. A subsequent
compact 30-sample check showed the same unblocked conditions. No DLL/settings
change or new candidate. This closes the requested activity-gate check; the
remaining investigation still needs to connect native updates or final draw
state to the moving-object symptom, not infer a fix from a field difference.

#### Existing v21 capture reanalysis — cross-pass cancellation mismatch confirmed

The earlier negative conclusion was too broad. Checking each draw's own
`C * V_native == V_eye` and folded projection does NOT check that early world
depth and later object draws share one corrected view. Extended the external
analyzer with chronological same-frame comparisons against the preceding
successful two-eye early-camera sync (kind 4, result 2). No new game capture.

8064 comparisons across 168 sampled frames have identical raw native V.
3517 draws across 102 frames nevertheless differ from the early corrected V
by more than 0.001 in a matrix element. Maximum forward-direction difference
is 2.57406766 degrees. For every changed-view comparison, early-view yaw minus
later-view yaw equals later observed cancellation credit minus early credit
within 0.000015220 degrees. Headset-relative render correction is therefore
being recomputed with different cancellation bookkeeping for the SAME native
camera packet during one frame. This is a concrete shared timing defect, not
evidence that individual prop matrices need another spatial correction.

Example frame 3148: raw native yaw remains -54.2682299 degrees. Early sync uses
credit -10.9776943 degrees and produces view yaw -51.6793790; later prepared
object camera uses credit -8.3888272 and produces yaw -54.2682335. Yaw gap
2.5888545 degrees matches the credit advance; forward-vector angle is
2.4608737 degrees because of pitch. The prior informal maximum of 2.46 degrees
was this example, not the full capture maximum of 2.57406766 degrees.

Important qualification: some unchanged prepared cameras also have a changed
credit at draw-entry observation. Credit is sampled after preparation, not
atomically inside the patch. Do not assert every native update immediately
changes every prepared buffer. The regression tests cover this distinction
and prohibit matching a draw against a future sync or a different frame.
Six external analyzer/ABI tests pass. The capture still records CPU prepared
state, not a guarantee that later draw overrides leave GPU bindings unchanged.

Code path: native angular hook updates sCullInjectedYaw in +28BFC0;
GetCullingCancellation returns the live accumulator in active native mode;
PatchMappedVRCameraData reads it anew on each camera construction, including
the early private camera and subsequent cb1 packets. Both operands may be
valid independently yet belong to different native updates. The native
camera packet stays unchanged while cancellation advances.

Next implementation must bind render cancellation to the camera generation
being consumed and reuse it for all corresponding passes and both eyes.
Merely returning a guessed two-frame-old credit is rejected: native updates
and render recording do not have a proven fixed delay. Freezing the first
credit for a render frame would address the measured cross-pass discrepancy
but by itself does not prove that credit belongs to the native camera; do
not call that complete native/mouse parity or a finished solution. Preserve
genuine body-turn credit separation, scripted ownership and recenter epochs
when identifying the production/consumption boundary.

Static boundary check: +69B900 is not an unconditional publication event.
manager+74 can skip it; the effector descriptor loop can also jump to the
unlock/return path before copying the supplied basis. Its normal path copies
position to manager+38, axes to +44/+50, normalizes them, writes shared camera
matrices and rebuilds frustum data before unlocking. Thus a hook that blindly
records current credit after every invocation could label an unchanged older
camera with newer credit, recreating the same bug. A production association
must distinguish actual publication, including effector ownership, rather
than just reuse the earlier basis-hook call boundary.

No C++ change, Release candidate, deployment, or additional headset request
in this analysis step. Metro is closed. Rehashed installed DLL: exact accepted
3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985.
Runtime diagnostics remain removed and no settings or branch merge changed.

#### Camera-credit lineage design and offline tests

Followed the actual publication and copy instructions, without a live probe:

- Normal +69B900 calls rigid constructor +D560 at +69BB4F (return +69BB54).
  Effector processing independently calls it at +69CF45 (return +69CF4A).
  Both write the native matrix at +D07730. Hooking actual construction avoids
  falsely counting +69B900's early-return paths as publications.
- +D560 returns its destination in RAX. Its native matrix is column-major;
  CB1's affine view is the transposed 3x4 representation. Do not compare the
  first 12 native floats directly with CB1.
- +7DF1E4 invokes +D5350 with destination rdi+60, source rdi+3B0: the OLD
  buffered camera is copied first. +7DF1F7 then invokes it with destination
  rdi+3B0 and source +D076F0: the NEW published camera is copied second.
  For the previously observed singleton, corresponding matrix fields are
  +D22F20 and +D23270 (camera-block bases are 0x40 bytes earlier).
- Correction to prior shorthand: +D5350 is not merely a 704-byte copy.
  It copies the initial 0x100 bytes, calls +D1220 on the following region,
  then copies scalar fields through +34C. Its destination stride is 0x350.
- After the second copy, an optional +650920 path can rebuild the buffered
  camera using +D560 at +650A78. A sidecar must invalidate or explicitly track
  such rewrites; stale metadata must not survive a matrix mismatch.
- Multiple main-view command callsites explicitly supply +D22F20 to +D6BE0,
  including +811FE4 and +817D54. Conversely the rejected +846F2E call supplies
  +D04070, not that main buffered matrix. Do not broaden its scope based on
  the earlier misleading "camera command" label.
- +D6BE0 emits opcode 0x70000014 at +D6F2C and its 64-byte payload at
  +D6F4A..+D6F6A. Recorder+10 is the command storage pointer; +18 is its DWORD
  cursor. Allocation/flush can occur before emission, so a pre-call storage
  pointer is not a reliable identity for the resulting command.
- Replay case +7EEAA9 copies the payload into stack storage and calls virtual
  slot +18 at +7EEAE1, reaching +7E44D0/+7E09A0. The setter's RDX therefore
  does NOT retain the command-buffer address. A metadata lookup keyed by that
  temporary pointer would be wrong. Replay can use immutable associated
  metadata without changing the late camera matrix or steering CPU culling
  there; native look/body state remains the upstream steering mechanism.

Implemented executable OFFLINE model `Tools/native_camera_lineage.py` with
ten tests. It propagates an immutable generation/epoch/credit beside exact
camera copies and recorded commands. Tests cover old/new copy order; a future
native update during render; both eyes/passes; identical matrices with different
body/head decomposition; unobserved camera rewrites; unknown/partial copies;
nonfinite values; layout conversion; recenter/load/toggle epoch invalidation;
and command-buffer address reuse represented by different batch generations.
All ten pass, as do the six v21 analyzer tests. The model is single-threaded,
not a C++ implementation, runtime validation, or a headset-ready candidate.

The remaining integration requirement is concrete: preserve command identity
across recording/replay, including allocation, reuse and execution boundaries,
and synchronize sidecar publication/copy with those native events. Neither a
matrix-nearest-match search, a pointer-only command lookup, nor reading the
current buffered global at replay is sufficient. The model intentionally
returns unknown for an untracked/mismatched command instead of borrowing the
latest credit. Production handling of unknowns must be defined before wiring
it into native follow; no speculative fallback was added to a play build.

No runtime hooks changed, no Release DLL produced or deployed, no new headset
capture requested, and no merge. Exact accepted installed DLL remains the
rollback. This step establishes the transfer algorithm and concrete native
boundaries; it does not claim the moving-object symptom is fixed.

#### Fixed-capacity C++ command metadata component

Implemented `DirectX11/NativeCameraCommandCredits.h` and an optimized x64
C++ test executable. This is the command-lifetime component, NOT installed
native hooks or a completed camera fix. No production file includes/calls it
yet. The distinction matters: these tests prove storage/transfer behavior,
not that the native call boundaries have been connected correctly at runtime.

New static evidence: +D6BE0 can flush from inside its capacity check. At
+D6EAD it may call +7EE2E0 synchronously; alternatively +D45F0 queues the old
buffer pointer and exchanges it for a pooled buffer. Only afterward does it
emit the new camera command. The metadata adapter must capture source credit
before the call, then identify the emitted payload from the POST-call recorder
pointer/cursor. It must never hold its own metadata lock across the original
recorder/interpreter call, which can reenter execution.

The C++ component keeps up to 64 buffer identities and bounded camera entries
per 0x8000-byte buffer. Recording is allocation-free; replay takes an independent
snapshot and immediately retires the stored buffer identity, permitting later
reuse without mutating an in-flight replay. Serial identities stay monotonic
across resets. Unknown-camera entries preserve exact command order; a mismatch
poisons the replay rather than searching ahead for a matching picture.
Epoch mismatch, overlap, unobserved reuse, out-of-bounds/alignment failure and
capacity exhaustion fail closed. A buffer containing no camera commands is
legal; an unexpected camera setter in that replay is not.

`Tools/test_native_camera_command_credits.cmd` builds the tests with MSVC x64
/O2 /W4 /WX and executes them. All 98 assertions pass, including separate
nested/in-flight snapshots, identical pictures with different credit, unknown
views, epochs, buffer reuse, exhaustion, incomplete replay and null input.
The ten Python lineage tests and six capture-analysis tests also pass.
Artifacts are under Builds/native-camera-command-credit-integration-20260906.
This is not a Release game DLL build or a performance comparison.

Still required before deployment: native adapters for publication and copies,
post-emission recording, interpreter entry/exit and ordered camera replay;
appropriate synchronization and thread-local camera lifetime; explicit runtime
handling of missing metadata; then coverage verification against real early
world/object packets. No C++ hook was enabled, installed DLL changed, extra
headset capture requested, or branch merged in this step.

#### v22 — runtime camera-credit integration candidate

Wired the sidecar into five signature-checked functions: +D560 native matrix
publication, +D5350 camera copies, +D6BE0 command recording, +7EE2E0 execution,
and +7E44D0 ordered camera replay. Their native arguments, output matrices and
commands remain unchanged. Source metadata is eligible only for the known
normal/effector publication callers in active, non-yielding native mode.
Other publications invalidate the corresponding known slot. Exact source and
destination comparisons reject stale or unobserved copies.

Recorder metadata is captured before calling Metro and attached to the emitted
packet using the post-call buffer/cursor. Every camera command gets an entry,
including unknown cameras. Interpreter entry detaches an independent snapshot;
thread-local fixed storage supports eight nested invocations without repeated
large stack allocations. Replay advances in exact camera-command order and
never searches for a matching matrix. Epochs invalidate old-origin credits on
both-grips mode changes and RecenterAiming. The last engine camera/credit is
retained across batch boundaries, including a camera changed by nested replay.
Depth overflow, incomplete replay and ambiguous recording are counted failures.

HackerContext now tries the replay-associated cancellation against the actual
raw affine view (native 4x4 transposed, tolerance 0.0001 for reconstructed early
camera roundoff). A match supplies the SAME yaw/pitch pair to view correction
and shake measurement. Native look/body steering and genuine-turn credit
separation are unchanged. This is metadata for the existing upstream steering,
not a new late matrix-only method to steer Metro's culling or interactions.

IMPORTANT candidate limitation: on absent/ineligible/mismatched metadata the
query currently leaves the prior cancellation path in place; it does not
invent a credit or force sprint/movement. This fallback can retain the known
v20 timing defect on uncovered packets. Therefore v22 is a coverage and visual
integration test, NOT proof of complete parity or a final deployable solution.
Matches/misses/faults are counted in memory and summarized only on epoch-reset
events (including both-grips toggle). No periodic camera trace, file output or
GPU readback was added. These temporary validation counters/logs must be removed
after the integration is accepted. Any fault or sustained ordinary-gameplay
misses require investigation rather than declaring the whole path fixed.

Verification: all five prologues match the archived mapped executable. Initial
review caught two missing 0x40 REX prefixes in proposed signatures; corrected
before deployment. Actual adapter C++ is compiled into a mocked-native test
harness: 48 assertions pass, including synchronous flush/replay inside recording,
future native updates before replay, identical-matrix unknown cameras, epoch
changes, unobserved rewrites and depth overflow. The fixed-storage component
passes 98 assertions, plus ten Python lineage and six capture-analysis tests.
Real native ABI, ownership coverage and rendering-thread coverage still need
the deliberate game test; mock tests do not substitute for it.

Release x64 MSBuild /t:DirectX11 succeeded. Initial full recompilation reported
the pre-existing KeyOverride C4250 warning in HackerDXGI.cpp; the final rebuild
had no warnings/errors. No headset smoothness or 72 FPS equivalence claim.

Archived under Builds/native-camera-credit-lineage-v22-20260906:

- candidate SHA256: 74EE1B910E8AF0681CA3FEB8BD234BBDD054E4286E4A3DEB7EABE52718B66883
- exact fallback SHA256: 3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985
- source snapshots and Restore-baseline.ps1 (checks Metro is closed, verifies
  fallback and installed identities, then restores and rehashes exact baseline).

With Metro confirmed closed, deliberately deployed this experimental candidate
and verified the installed hash equals the candidate. No INI/settings changes
or merge. Starts in legacy mouse-driven mode; both grips select native mode
and both grips again return to legacy and emit the coverage summary. Next test:
affected area, physical turns in native mode, observe formerly moving props,
then switch back and report the result. Assistant reads the runtime log.
The exact fallback remains archived, not rebuilt, for immediate restoration.

#### v22 first positive headset report and coverage verification

User reports: "Looks like things are no longer moving with the headset. Also
the merchant's screen is forced the right way now." Treat this as a positive
headset result for those two observed symptoms, not a blanket validation of
Defense, performance, movement, scopes, effects, or all scripted ownership.

Assistant retrieved the log directly. All five adapters installed. Two completed
native-mode intervals report 292448 matches / 92 misses / 0 faults and
832706 matches / 304 misses / 0 faults. Combined: 1125154 matched render-credit
queries, 396 unmatched queries, zero reported adapter faults; match coverage
99.9648172% of eligible queries. These counts are camera-patch calls (including
eyes/multiple writes), NOT unique frames or GPU draws. Both intervals ended
with explicit both-grips return to legacy mouse mode. Thus the positive result
was obtained with the new path genuinely active, not just the DLL loaded.

The aggregate counters do not identify when/why the 396 misses occurred.
Do not assume they are harmless startup/menu transitions or declare complete
coverage. Their fallback behavior and the remaining regression list still need
review before diagnostics are removed and a clean final candidate is accepted.
The archived runtime also contains older periodic diagnostic output outside
the new adapter; this must be audited for final play-build cleanup, not ignored
because the new adapter itself only emits event summaries.

Archived a non-overwriting log snapshot as
Builds/native-camera-credit-lineage-v22-20260906/first-positive-headset-runtime.log.
Installed DLL still hashes to 74EE1B91...; exact fallback rehashed to 3EAB9B24...
(full identities above). No DLL/source/settings changes or merge in response
to this report; preserve the successful candidate unchanged while reviewing
the remaining gaps. No new headset capture requested for this verification.

#### v23 — bounded miss classification, correction unchanged

The v22 aggregate log cannot distinguish the 396 unmatched queries by cause or
time. No retrospective classification is possible from those counters alone.
Do not loosen 0.0001 tolerance or assume transition-only misses just to reach
100% coverage. Added temporary classification WITHOUT changing the successful
camera construction, matrix association, acceptance threshold or fallback.

NativeCameraCreditMisses.h holds per-reason counts/first/last frames and at most
32 example records, deduplicated by reason/frame: unavailable credit, old epoch,
nonfinite view, rotation mismatch, translation-only mismatch. Samples contain
maximum element gaps, not full camera traces. Invalid credits show generation
and creditEpoch zero rather than implying provenance from stale TLS storage.
Unavailable-credit remains an honest broad category, not a claimed root cause.
Also retains at most 32 native ownership transition frames so misses can be
compared with actual entry/release events. No gameplay decision reads this data.

Data stays in fixed memory during play; summaries/examples are emitted only
when a mode/recenter epoch is reset, after releasing the metadata lock. No
periodic trace, per-frame file logging or GPU readback introduced. All this new
instrumentation remains temporary and must be removed from the final candidate.
The actual adapter harness now passes 138 assertions, including unchanged
threshold behavior, untouched output parameters on rejection, per-reason counts
and sample-cap enforcement. Storage suite still passes 98 assertions. Release
x64 build succeeded with no warnings/errors in this rebuild.

Diagnostic-cleanup audit located older active periodic blocks in HackerContext:
watch UI/signature logging around BeforeDraw, instance-transform summary in
ResetVRInstanceXformFrameTracking, StereoTwin timing/run summaries, gas-mask
visual census, and watch-child reparent messages. VRPose's rendered-vs-engine
aimcull diagnostic is also periodic. These blocks are not needed for credit
association and should be removed/compiled out, along with their diagnostic-only
timers/counters, before the final play build. This step only audits them: it
does NOT alter the accepted wrist timer's placement/update/rendering logic or
resume parked Defense work. Cleanup should be separately reviewed and tested.

Archived exact v22 (74EE1B91...), exact original baseline (3EAB9B24...), v23
diagnostic and source snapshots under
Builds/native-camera-credit-miss-breakdown-v23-20260906. Restore.ps1 defaults
to the exact baseline; -Version PositiveV22 restores the reported-good candidate.
Both options verify source/current/destination hashes and require Metro closed.

With Metro confirmed closed, deliberately deployed v23 and verified installed
SHA256 58BB6D105EA936E818DE1D8E759041A5B977B2EFA37114A4CBBECC6279659620.
No settings changes or merge. This is the same correction with a more useful
bounded diagnostic, not a new visual fix or performance-equivalence claim.
Next normal regression session can collect the breakdown while checking real
physical/body turning, walking/sprint, pickups and merchant handoff. Both grips
select native mode; switching back after the session emits the evidence that
the assistant will retrieve. No separate tiny-angle test is requested.

#### v23 positive headset report — misses localized to ownership handoffs

User reports "Everything looks good as far as I can tell." This is positive
feedback for the tested session; do not infer that every item in the original
regression list, specifically Defense carry/release and 72 FPS comparison,
was exercised or independently validated.

Assistant retrieved the runtime log directly. Two native intervals report:
201740 matches / 492 misses / 0 faults, and 1270582 matches / 595 misses /
0 faults. Total 1472322 matches, 1087 misses, zero adapter faults; 99.9262255%
eligible-query coverage. Every miss is classified no-credit. No old-epoch,
nonfinite-view, rotation-gap or translation-gap rejection was recorded.

Each interval contains ten distinct miss-frame examples, below the 32-example
limit, so the frame list was not truncated by that cap. First interval:
2768..2773, 5971, 6093, 6179, 7129. Ownership returns at 2768/5971/6093/6179/7129;
the initial short native interval yields again at 2773. Second interval:
8233, 12462, 18601, 22992..22998. Ownership returns at 8233/12462/18601/22991;
the last short native interval yields again at 22998. Thus all recorded miss
frames fall on the ownership-return frame or within these short handoff
windows (at most seven frames after the observed return), not scattered through
steady native gameplay. Existing aggregate v22 data cannot be retroactively
classified, but v23 provides this direct temporal evidence for its own run.

Interpretation: consistent with buffered cameras still carrying ineligible
pre-handoff metadata while native ownership resumes. No evidence here of the
former same-camera/different-cancellation failure on matched packets. This
does not prove each fallback query visually harmless in every authored scene,
nor prove all special scenes covered. Given positive headset feedback and
zero matrix/sequence errors in this run, preserve the working correction and
proceed to separately reviewed diagnostic cleanup rather than relaxing the
matching threshold or adding another camera/movement workaround.

Archived log as
Builds/native-camera-credit-miss-breakdown-v23-20260906/positive-headset-runtime.log.
Reverified installed v23 58BB6D10..., archived positive v22 74EE1B91..., and
exact original baseline 3EAB9B24... (full hashes above). No C++ changes, DLL
replacement, settings changes or merge in response to this report. Next step
is a clean candidate with temporary diagnostics removed, retaining reversibility
and requiring headset regression/performance validation before acceptance.

#### v24 — native thumbstick body-turn delivery (headset test pending)

User identified remaining device-prompt fighting specifically during right-stick
turning. Inspection confirmed this separate path still emitted SendInput mouse
movement even with native HMD follow selected. Replaced only its native-mode
delivery; legacy mode and combined SendLookAxes remain intact for both-grips A/B.

Use Metro's own +28B280 look-input processor directly on the native update thread,
with player+E20, the existing horizontal turn units, zero pitch and zero wheel.
Validated its prologue and relative +28B4C2 call to +28BE80 before installation.
This processor preserves native sensitivity, weapon response modifiers, ownership
gates and action processing, including ordinary pending yaw at player+F4C.
Real body input runs before the normal angular update and before enabling the
private HMD-follow scope. Only the separate HMD delta receives injected-yaw
credit. No synthetic turn mouse events or virtual right-stick injection in
native mode; no raw-angle override, sprint forcing, gate bypass or fake activity
timestamp. Native activity updates caused by actual user input are retained.

Rejected shortcut alternatives: hiding prompts would leave device arbitration
untouched; raw angle assignment would bypass native response; a shared virtual
stick rate channel would repeat the earlier cancellation ambiguity. Direct use
of the existing native input processor retains those semantics without Windows
mouse delivery. Prompt behavior still requires a headset test: removing the OS
event does not prove no indirect device-arbitration side effects exist.

Added an SRW-locked bounded one-shot intent mailbox. Latest smooth units replace
an unconsumed sample; a latched snap stays pending until consumed or cancelled.
Neutral stick, early controller-input returns, recenter and A/B transitions
clear it. Blocked/settling native updates consume and discard requests rather
than accumulating a catch-up turn. Native smoothing/residual state is untouched.
Original deadzone, smooth tx*30 units, snap conversion and latch remain unchanged.
IMPORTANT: producer/update cadence can differ; superseded smooth samples are
not accumulated as OS input might be. Delivery timing and turn speed/feel need
comparison, not an assertion of identical behavior. No guessed frame timeout.

Verification: optimized C++ tests pass (98 storage, 138 adapter, 112 new body-turn
assertions); Python native-state 6, lineage 10 and instance-capture 6 tests pass.
Archived native hook signature checks pass. Release x64 build and diff checks
pass. These cover bounded delivery, snap/release/context cancellation and credit
separation, not live engine response, prompt arbitration or headset performance.

Archived source snapshots and three exact DLLs in
Builds/native-thumbstick-body-turn-v24-20260906:
- Candidate: F207818B51D2056A98C6F94307573E728FD5426468A8F1FA7A8637993003ECCC
- Positive v23: 58BB6D105EA936E818DE1D8E759041A5B977B2EFA37114A4CBBECC6279659620
- Original fallback: 3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985

With Metro closed and all source hashes checked, deployed the candidate and
verified the installed hash. Restore.ps1 defaults to the exact original fallback;
-Version PositiveV23 restores the last reported-good test. It requires Metro
closed and checks source/current/destination hashes. No settings changes or merge.

Next test: native vs legacy prompt behavior, smooth speed and release, snap if
used, simultaneous physical/body turns, walking/sprint and merchant ownership.
Assistant retrieves logs. Successful camera-credit correction and accepted
Defense timer are unchanged. Diagnostic cleanup remains pending separately;
this is not the final clean play build or a 72 FPS equivalence claim.

#### v24 positive turning-feel report — 2026-09-07

User reports "Yeah that feels good" after native thumbstick delivery deployment.
This confirms positive subjective turning feel for the tested session, not an
explicit confirmation that prompt switching is gone or that every regression
scenario/performance reference has been tested.

Assistant retrieved and archived the runtime log as
Builds/native-thumbstick-body-turn-v24-20260906/positive-feel-headset-20260907.log.
Installed DLL remains verified F207818B51D2056A98C6F94307573E728FD5426468A8F1FA7A8637993003ECCC.
Log confirms native body-input installation and both-grips native activation,
legacy comparison, then native reactivation. Completed first native interval:
297038 matched credit queries, 118 no-credit misses, zero adapter faults.
The two recorded miss frames (1588, 8764) coincide with ownership returns.
Second native interval has no closing summary in this snapshot; do not infer
its totals. No runtime camera fault evident in the completed summary, but these
counters do not measure prompt arbitration or input latency/turn speed.

No code, settings or deployment changes in response to this report. Keep v24
installed; preserve both rollbacks. Prompt behavior needs explicit user feedback.
Final diagnostic cleanup and remaining headset regressions are still pending;
no merge or claim of exact 72 FPS reference equivalence.

#### v25 — diagnostic cleanup, 2026-09-07

User explicitly confirmed v24 eliminated all input switching they could observe,
in addition to the positive turning-feel report. Record this as headset evidence
for their tested session, not proof of every input/action/chapter combination.

Cleanup preserves the native HMD/angular path, native +28B280 thumbstick delivery,
body/HMD credit separation, matrix/generation/epoch checks, command poison/failure
handling and replay depth guard. Removed v23 miss samples/ownership examples and
match/miss/fault counters from the runtime adapter. Calls to Commands::Record and
TakeReplay still execute and retain their internal invalidation behavior; removed
only the external counting of their boolean results. The effector observer is a
functional camera/viewmodel ownership source now, so it remains installed; only
its transition trace was removed and the stale diagnostic-only comment corrected.

Removed observed active periodic reports and associated hot-path work: rendered
vs engine aimcull camera reads/trigonometry; watch signature/surface/fallback logs;
gas-mask worn-visual census; watch-child transform reports; instance-transform
cumulative summary counters; stereo map timers, run histogram and counters, and
unused seen-cb0 set insertions. No watch texture identification, glyph capture,
timer fallback qualification, placement or rendering changes. No new GPU reads,
input tuning, walking/sprint workarounds, camera reanchors or hidden prompt fix.
Existing disabled RE/census/performance facilities remain disabled. Ordinary
startup/failure and A/B status messages remain available; this is not a globally
muted logger. Historic diagnostic header/tests remain offline, not included by
VRPose. Original diagnostic source is recoverable at d21b4de and older commits;
v24 DLL/log are archived beside the new candidate.

Verification: optimized C++ storage 98, cleaned adapter 128, body intent 112
assertions pass. Adapter count fell by ten diagnostic-counter assertions only;
behavior/rejection/order tests remain. Python native-state 6, lineage 10, instance
capture 6 pass; all five archived adapter signatures pass. Release x64 build
passes. Tools/verify_native_camera_play_build.ps1 verifies absence of 15 retired
report strings and presence of native installation markers in both built and
installed DLL. This binary smoke check is not an exhaustive reachability proof.
Initial cleanup build caught a dangling diagnostic-only if after counter removal;
removed that if, rebuilt successfully. Initial Python module-style invocation
could not resolve sibling imports; direct script invocations all passed. No
failed intermediate build was deployed.

Archive: Builds/native-camera-clean-v25-20260907, including source snapshots,
positive-v24-precleanup.log and exact DLLs:
- Clean candidate: 16E94C622B2E897FD3B065C52B208E71AB0A6B4334691E69DE14BD5576E54330
- Positive v24: F207818B51D2056A98C6F94307573E728FD5426468A8F1FA7A8637993003ECCC
- Original baseline: 3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985

Metro was closed. Verified installed v24 and both fallback sources, deliberately
deployed v25 and reverified the destination hash/binary smoke check. Restore.ps1
defaults to exact original Baseline; -Version PositiveV24 restores the latest
headset-positive native build. Both require Metro closed and verify hashes.
No INI/settings changes; still starts legacy, both grips selects native as before.
No merge. Cleanup is not a claim of performance equivalence to the exact 4AD471D3
72 FPS reference. Final headset regression remains required, especially Defense
carry/release and wrist timer, scripted/merchant ownership, physical/body turns,
walk/sprint, shooting/laser/effects/culling, ADS/scopes, loading/recenter and menus.
Assistant retrieves runtime logs; no request for user log inspection.

#### Post-v25 feedback and focused follow-up — 2026-09-07

User reports v25 feels good, fixed the Defense issues, and appears to improve
framerate. Positive headset evidence for their tested Defense behavior; subjective
framerate feedback, not measured equivalence to the exact 4AD471D3 reference.
Two remaining reports: objects disappear at the headset's upper edge, and the
weapon merchant initially forces the correct view but does not focus individual
guns while browsing as native Metro does. Preserve v25 as the working fallback.
Installed hash reverified 16E94C622B2E897FD3B065C52B208E71AB0A6B4334691E69DE14BD5576E54330.

Initial read-only investigation (no gameplay patch/deployment):
- Runtime log contains headset raw vertical tangents +/-1.3065 (about 105.1
  degrees total), and early native projections with P11=2.1143 (about 50.6
  degrees). Those early samples include loading/other views and do not establish
  the actual affected gameplay culling projection. Check live upstream coverage
  before assuming a blanket FOV increase fixes the report.
- IMPORTANT correction to the earlier note claiming 110-degree CPU FOV is
  configured: ForceWideCullingFov returns before its old 110-degree write.
  Current runtime does install the existing CPU-screen/object/cluster bypasses;
  do not infer a coverage guarantee from that stale unreachable FOV code/comment.
- Archived native image has trade_camera_track at +ADFEE8. Xrefs +2AE3C6,
  +2AE3D7 load it in configuration processing (stored at object+E90); +2AF37C
  serializes it. This is a promising native track lead, NOT yet proof of the
  live per-gun camera owner or a safe hook. No hard-coded weapon targets or
  broadened scripted-state flags have been introduced.
- Extended external read-only observe_native_look_state.py with optional
  --camera-context: native projections, FOV cvar, input-owner UI state and up
  to 32 effector descriptors. It keeps PROCESS_VM_READ only and the existing
  <=30 second bound; it performs no code injection, writes or GPU readbacks.
  Samples are not atomic across game threads. CLI/compile smoke checks pass.
  An initial startup sample has zero published projection/forward and cannot
  establish gameplay coverage. User asked to leave native mode at gun browsing
  for a labeled live sample; no replacement DLL or keyboard test needed.

#### Labeled weapon-vendor samples

Captured PROCESS_VM_READ-only samples from PID 41692, module base
140695506518016 after user reported ready, then after selecting a different gun.
Archived vendor-ready.jsonl and vendor-second-gun.jsonl in
Builds/native-vendor-coverage-investigation-20260907. Runtime log confirms native
mode last activated at frame 3541; installed v25 hash remains unchanged.

First selection native camera position (52.52919,1.65700,-12.69561), forward
(-0.021381,-0.857637,-0.513810). Second selection position
(52.61102,1.52846,-12.20122), forward (0.021749,-0.870953,-0.490885).
Both have a native camera pitched strongly down toward the display, unlike the
player look-state pitch (0.18116 then 0.24390 radians). These observations support
preserving the native camera track rather than constructing substitute gun aims.
They do not alone distinguish selected-track movement from all physical motion.

First sample effectors: 0x040C and transient 0x000C; second: 0x040C only. Neither
is in the existing authored-camera flag list. The 0x040C descriptor's pooled name
is default_true, NOT a unique weapon-vendor identity. Do not simply classify all
such effectors as authored without comparing ordinary gameplay and verifying
native semantics. Input owner remains the verified +ADD110 implementation;
input+E1=0, input+137=0, [input+190] begins 00 01 00; timed player owner absent.

Native global projection at +D07770 has P11=2.114322 (50.625 degrees vertical)
in both samples. The field at final camera+190 is not the same perspective
matrix (inverse-like layout); do NOT interpret its diagonal as the culling FOV.
Archive disassembly places global view-projection construction before native
frustum helper +8E93C0 at +69D196, followed by +8E92A0. Still need the ordinary
gameplay projection and affected visibility path before selecting a coverage fix.
No C++ or installed-DLL changes. Requested one outside-vendor comparison.

#### Outside-vendor comparison and selected-gun framing clarification

User clarifies the defect is failure to focus/frame the selected guns themselves,
not merely the initial vendor-facing orientation. Outside-vendor sample archived
as outside-vendor.jsonl in the same investigation directory. Native global P11
remains 2.114322 (50.625 degrees vertical) in ordinary gameplay. This confirms the
narrow upstream projection is not only a loading/vendor artifact, but does not
yet prove which visibility stage rejects the reported upper-edge objects.

The identical 0x040C/default_true effector remains outside the vendor, alongside
0x060C (pooled name anims\\object\\drezina\\up_camera_20). Therefore adding 0x040C
to the authored-camera list is REJECTED: it would also classify ordinary gameplay
as authored and threaten the working HMD/Defense behavior. Input+E1 changes from
0 at the vendor to 1 outside, but this is not yet proven a vendor-specific owner.
The UI+190 first bytes remain 00 01 00. Do not use that byte pattern as a vendor
classifier. Native source pitch returns to agreement with player look outside.

Render-path inspection identifies a concrete mechanism that can discard the
selected-gun vertical framing: HackerContext's additive view build explicitly
sets kBodyPitchFromEngine=false and uses GetScriptedPitchOffset instead. That
function returns the authored contribution only for recognized native ownership
or its release easing. A native gun-focus pitch can consequently exist upstream
without reaching the rendered HMD direction. This is a mechanism consistent with
the samples, not a fully proven vendor-specific fix. Globally enabling engine
pitch would revive the prior head-coupled world-motion problem; do not do that.
Need identify/preserve the real selected-gun camera owner or its native framing
contribution without weakening ordinary HMD credit separation.

Also traced input+E1 writes to native +2996F0: derived from its two arguments,
not a unique vendor identity. No guessed focus coordinates, zoom, broad effector
classification, FOV cvar override or runtime DLL change. v25 remains installed.

#### v26 — bounded main-world visibility coverage candidate

Continued investigation found a more precise early visibility boundary. The
main-world preparation at +812AFD loads +D22FA0 (previous buffered camera's VP,
base +D22EE0 plus C0), calls +8E93C0 at +812B0F with mask 1F, then finalizes that
frustum through +8E92A0. The constructor also has eleven other callers, including
other camera/render paths; do not widen them indiscriminately. Also corrected
another stale diagnostic comment: archived +812D7B calls +802990, not +802580.
Do not revive that old visibility-context bypass based on its comment alone.

v26 hooks the native frustum constructor but changes arguments ONLY for return
address +812B14, source pointer +D22FA0 and mask 1F, while native mode is selected,
stereo projection valid and native follow not yielding. It checks constructor
bytes, selected relative call and RIP-relative source against the archived
image before installing. All other calls are pass-through.

NativeVisibilityCoverage.h validates a conventional native perspective matrix
and checks that the supplied VP equals the corresponding buffered V*P. It uses
the runtime's two headset projections to derive a conservative roll-independent
corner envelope, then scales only VP's lateral columns in a private aligned
stack copy. Native depth columns/near-far clipping and original plane mask stay
unchanged; no global matrix/cvar write, no disabled plane or all-visible mask.
The original constructor/finalizer still creates Metro's actual visibility
frustum. This acts before world-list submission, not on the final picture.

The full-roll envelope deliberately over-covers an upright headset (approximately
125 degrees symmetric coverage for the recorded headset), potentially submitting
more geometry. It does not prove performance parity or cover every independent
portal/effect/occlusion path, stereo-position offset or native-vs-render camera
heading mismatch. It is a targeted candidate for the reported upper-edge loss,
not a promise that every culling symptom is fixed. Keep native FOV cvar unchanged:
static xrefs prove it is also consumed by look sensitivity, so simply reviving
the old 110-degree cvar override would change more than visibility.

Verification: 2893 optimized C++ coverage assertions (both eyes' corners across
360 roll angles, unchanged depth columns, invalid/mismatched inputs rejected);
20 actual-adapter checks under a mocked native engine (source/caller/mask/mode/
ownership/stereo gating, pass-through and unmodified shared matrix); existing
98 command-credit, 128 camera-adapter and 112 body-turn assertions pass. Archived
constructor signature, relative call and VP source checks pass. Release x64
build succeeds; clean-build binary check still passes. No per-frame logging,
camera traces, GPU readbacks, settings changes or merge. Math/mock tests do not
prove live hook selection, upper-edge symptom resolution or headset performance.

Archived and deliberately deployed with Metro closed, checking source/fallback/
installed hashes. Builds/native-visibility-coverage-v26-20260907 contains:
- Candidate: 78DF13EFF55DDA9E6292B98A156FE97FB12E4C46D0EE96DD4C20BBD957784020
- Exact positive v25: 16E94C622B2E897FD3B065C52B208E71AB0A6B4334691E69DE14BD5576E54330
- Exact original baseline: 3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985

Restore.ps1 defaults Baseline; -Version PositiveV25 restores the latest Defense-
positive build. It requires Metro closed and checks hashes. Both-grips native/
legacy selection remains unchanged. Next headset check is upper-edge visibility
and frame pacing in the affected area, not another small-angle rotation probe.

Vendor selected-gun framing is explicitly NOT changed by v26. Extended the offline
image inspector with absolute data-pointer xrefs; native +2996F0 is input-owner
virtual +78, but generic calls to virtual +78 are not proof of vendor ownership.
The +040C/default_true broad classifier and globally restoring engine pitch remain
rejected. Need isolate native gun-framing ownership before modifying that path.

#### v26 headset result and deferred VR-menu option — 2026-09-07

User confirms the top-edge disappearance is fixed and reports a small framerate
reduction. This is subjective headset feedback, not a measured comparison with
the exact 72 FPS reference. Vendor selected-gun framing remains unresolved.

Deferred at the user's request until the planned VR-menu revamp: add a saved
"Expanded visibility" toggle, proposed default ON. ON retains v26's wider
main-world visibility envelope; OFF restores the narrower pre-v26 visibility
coverage for a potential performance benefit, with possible renewed edge pop-in.
The toggle should gate only the visibility expansion, not native HMD/body turning,
input-switching fixes, camera-credit handling or Defense fixes. It does not
disable culling. Explain the visibility/performance tradeoff in the menu.

Documentation only: no setting implemented, no source/build/deployment change.
Current visibility behavior and exact archived rollback DLLs remain unchanged.

#### v27 — native-only camera mode — 2026-09-07

User requests retiring mouse mode after positive native-mode testing, then
returning to vendor focus. Native selection now starts enabled; successful native
hook installation seeds credit history, clears body-turn intent and retains the
existing 32-update settling window. Removed both-grips camera toggles, the
thumbstick SendInput fallback, synthetic HMD yaw/pitch delivery/prediction tail,
and its cursor-pinning helpers. Installation failure does NOT silently emit mouse
look. Restore a verified DLL if native installation is unavailable.

Preserved native angular/body input processing, actual rotation credits, camera
command/replay lineage, measurement and authored handoff logic, direct rendered
HMD pose, Defense timer, and v26 visibility expansion. Mouse button actions and
unrelated menu controls remain; historically disabled injection-test and parked
motion-aiming code remain disabled, not additional selectable camera modes.
The settings toggle remains deferred. No vendor-framing change in this build.

Release x64 succeeds. Existing optimized C++ checks pass: 98 command-credit,
128 camera-adapter, 112 body-turn, 2893 visibility and 20 visibility-adapter.
Tools/verify_native_only_camera.ps1 checks retired delivery/toggle removal,
native startup selection, retained installation paths and compiled native-only
marker; existing 15 retired diagnostic markers remain absent. These checks do
not replace a cold-start headset test now that manual activation is removed.
No new runtime traces or per-frame logging. No performance-parity claim.

Deployed with Metro closed; built/installed SHA256:
80D4B05118529026D9F40C4262696B5D99EC501EA04A29F23C5697E994DB36E5.
Builds/native-only-camera-v27-20260907 preserves this candidate, source snapshot,
exact positive v26 (78DF13EF...) and original fallback (3EAB9B24...). Full source,
rollback and installed hashes checked. Restore.ps1 defaults PositiveV26, restoring
the proven A/B build; -Version Baseline restores the original. Requires Metro
closed and refuses unknown installed/source hashes. No merge.

Next vendor investigation still needs a specific native gun-framing owner or
contribution: sampled native gun-selection pitch changes were discarded by the
rendered authored-pitch gate, but broad default_true/input-E1 classification and
global engine-pitch passthrough remain rejected. Keep that fix separate from v27.

#### v28 — native trade/customize camera-owner candidate — 2026-09-07

Static tracing identifies the native trade/customize controller rather than a
broad camera effector. Its configuration contains trade start/finish, object and
customize browsing callbacks, camera selection/position controls,
cam_track_accrue/falloff, and trade_camera_track. The controller's adjacent
vtable entries +B7A8D8/+B7A8E0 point to paired activation/deactivation methods
+42DDE0/+42DE3C. Their native bodies register/unregister the associated input/UI
ownership. This is materially narrower evidence than input+E1 or the persistent
0x040C/default_true effector rejected above.

NativeVendorCameraOwner.inl validates both method prologues and both relocated
vtable pointers before hooking. It hooks deactivation first and only allows the
activation hook to latch ownership after both hooks succeeded, making a partial
installation fail closed. Activation stores the exact controller pointer;
deactivation clears only that same pointer, so a stale/unrelated close cannot
release a newer owner. First activation/deactivation are logged once for this
temporary candidate; there is no periodic/per-frame logging.

While this exact owner is active, it joins the existing authored-camera handoff:
native HMD angular updates pause, upstream camera construction passes Metro's
own basis through, direct locomotion correction stands down, and the already-
validated rendered scripted-pitch path consumes the live engine-minus-head
offset. No gun target, angle, FOV or camera matrix is guessed. The player should
therefore retain additive HMD freedom around Metro's own selected-gun framing,
the same model used for other forced views. Outside trade/customize, v27 behavior
is unchanged. Visibility expansion and native-only input remain intact.

Verification: the archived image confirms the configuration/string association,
method bytes and adjacent vtable slots. Twelve optimized mocked adapter checks
cover install order, activation, stale-owner close, owner replacement and exact
release. Existing 98/128/112 native camera/body and 2893/20 visibility checks
pass; Release x64 succeeds; native-only and 15 retired-diagnostic binary guards
pass. Static and mocked checks cannot prove the lifecycle methods fire during
the live weapon-vendor screen or that Metro's selected-gun framing is visually
comfortable in the headset.

Deployed deliberately with Metro closed. Candidate/built/installed SHA256:
68496A1509F69326991E8DB1D1F3D2DC35CBC2547E34BCAA52A1137BA7E84A87.
Builds/native-vendor-camera-owner-v28-20260907 preserves the candidate/source,
exact positive native-only v27 (80D4B051...), v26 A/B (78DF13EF...) and original
fallback (3EAB9B24...), with guarded Restore.ps1 defaulting to PositiveV27.
No settings change, no merge. Headset test: enter the weapon vendor, browse
between guns, confirm Metro now changes the focal framing, then exit and ensure
ordinary HMD/body movement immediately returns unchanged. Assistant retrieves
the log if the visible result is absent; user need not inspect it.

#### v28 result and v29 late-install correction — 2026-09-07

User reports no change. Their non-VR screenshot clarifies the expected native
behavior: every vendor category uses a dedicated near-overhead inspection view
centered on the currently selected item (shown with a pneumatic weapon on its
table), not merely a small pitch adjustment to the player camera. User also
confirms the defect is not limited to the weapon vendor.

Retrieved the runtime log directly. It reports only
"vendor camera owner: signature/vtable rejected; disabled" and contains no
activation event. Therefore v28 never exercised the proposed ownership handoff
and its visible result does NOT reject the trade-controller hypothesis. Metro's
trade UI code/vtable was unavailable when the early player-camera installer ran;
the v28 guard correctly made no hook and no camera change.

v29 leaves the exact signature/vtable checks and owner behavior unchanged, but
an unavailable class remains state 0 rather than becoming permanently rejected.
UpdateMotionAiming performs only that bounded read-only validation until Metro
exposes the class, then installs once. There is no log or hook while unavailable;
after success the existing install marker and bounded first enter/exit markers
provide evidence. Hook failure after validated addresses remains terminal/fail-
closed. Extended the mocked test to prove an early unavailable attempt makes no
hooks and can install successfully later: 14 checks now pass. Existing
98/128/112 and 2893/20 checks, Release x64, native-only guards and 15 retired-
diagnostic checks pass. No non-VR run is needed unless v29 installs and the live
result still differs; the supplied screenshot is an adequate visual target.

Deployed with Metro closed. Candidate/built/installed SHA256:
78B141AE094C6F1E9CB98E336B2870E630EE15F7EC2950E529969FD53C1CF060.
Builds/native-vendor-camera-late-install-v29-20260907 preserves candidate/source,
exact positive v27 (80D4B051...), rejected/no-op v28 (68496A15...) and original
fallback (3EAB9B24...), with guarded restore defaulting PositiveV27. No merge.

#### v29 result and v30 exact live-prefix correction — 2026-09-07

User again reports no visible vendor change. Retrieved log contains neither the
vendor-owner install marker nor activation, proving v29 also remained inert.
At the user's offered non-VR reference run, captured the active overhead vendor
view externally with PROCESS_VM_READ only. The native camera was at
(28.10678, 1.58213, -0.37179), forward
(-0.56921, -0.81237, 0.12670), about 54.3 degrees downward. This independently
confirms Metro already creates the required overhead view and closely matches
the steep native vendor camera captured under VR. No reconstructed target or
additional non-VR run is presently needed.

The same live read exposed why validation never completed: actual activation
bytes begin `40 53 48 83 EC 20 48 8B D9`, and deactivation begins
`40 53 48 83 EC 20 80 79 60 01`. The archived instruction listing represented
the leading 0x40 REX prefix as part of `push rbx`; v28/v29 signatures incorrectly
started at the following 0x53. Live relocated vtable entries still exactly equal
+42DDE0/+42DE3C, confirming the selected class/functions. v30 adds that one byte
to each prologue signature. Ownership behavior and late retry are otherwise
unchanged. The external sampler's temporary --vendor-owner-code option captures
only these bounded addresses once; remove/archive it with final diagnostics.

All 14 vendor-adapter, 98/128/112 native and 2893/20 visibility checks pass;
Release x64 and native-only/15-retired-marker verification pass. Deployed with
Metro closed. Candidate/built/installed SHA256:
266696813280737F88ECC702B88DE1B72F475D06D750357AF225B9BA819D9778.
Builds/native-vendor-camera-prefix-v30-20260907 preserves candidate/source,
exact positive v27 (80D4B051...), rejected inert v29 (78B141AE...) and original
fallback (3EAB9B24...), with guarded restore defaulting PositiveV27. Headset
vendor browse and exit behavior remain to be validated. No merge.

#### v30 vendor result and v31 diagnostic cleanup — 2026-09-07

User confirms v30 restores Metro's intended overhead selected-item view at the
vendor. Retrieved log independently confirms the corrected exact hook installed,
observed the native trade controller's activation and deactivation, and drove the
existing authored-camera acquire/release handoff. This validates the native
controller ownership path across the vendor categories tested; v28/v29 remain
rejected as inert signature experiments, not alternate implementations.

v31 changes no ownership, camera, visibility, input or Defense behavior. It
removes the temporary one-shot trade activation/deactivation messages and the
external sampler's temporary `--vendor-owner-code` switch, and updates stale
"legacy fallback" diagnostic wording to describe Metro's native authored source
camera accurately. The permanent guarded install/failure marker remains. The
native-only verifier now rejects those temporary diagnostics in both source and
binary.

Release x64 succeeds. Vendor 14, native command/adapter/body 98/128/112 and
visibility 2893/20 checks pass, along with play-build and native-only guards.
Clean candidate SHA256:
0B720BFF59EE42496101AE4D3AB0B20260D226EAA94AF5B207F78E2E4CD1C929.
Builds/native-vendor-camera-clean-v31-20260907 preserves the exact headset-
positive v30 (26669681...) as the immediate/default rollback, positive v27
(80D4B051...) and original fallback (3EAB9B24...), with a hash-guarded restore
script. No settings change and no merge. Broad final regression and headset
performance comparison against the older 72 FPS reference remain required.
Deployed with Metro closed; the installed hash was verified as v30 immediately
before replacement and as v31 afterward.
