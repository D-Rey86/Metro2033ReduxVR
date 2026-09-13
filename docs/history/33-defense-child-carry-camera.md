# Defense child-carry camera investigation — parked 2026-09-06

## Current disposition

The investigation is **parked, not solved**. All Defense-specific behavioural
changes have been removed from the active source path. The game has been
returned to the exact headset-validated 72 FPS mouse-follow DLL:

- installed/archive SHA-256:
  `4AD471D3387014AF2C6A150B8CF7AF5385C1AE1AA24D3688F4154FD054B64E57`
- archive: `Builds/pitch-compensation-combined-input-20260905/d3d11.dll`
- source checkpoint for the restored behaviour: commit `68cb4d1`, represented
  at the branch baseline by `e07c7d2`
- Defense work remains isolated on branch `codex/defense-fixes`; its diagnostic
  artifacts and candidate DLLs remain under `Builds/defense-*`.

Do not resume by blindly re-enabling the last measured-yaw/re-anchor patch. It
fixed the Defense symptom but changed global camera semantics and caused walking
and laser regressions.

## Exact player-visible symptom and boundary

Only the child-carry interval in the Defense level has reproduced the problem:

1. Before the authored scene where the child climbs onto the player's back,
   HMD yaw and smooth right-stick turning behave normally.
2. After pickup, the world partly follows the HMD when looking left/right.
3. Smooth right-stick turning becomes less responsive and continues for a
   noticeable interval after the stick is released.
4. When the child jumps off near the end of Defense, both symptoms immediately
   return to normal. The recovery occurs before the next level.
5. With the VR DLL disabled, the game's native right stick remains normal during
   the same carry interval. This is not an authored slow-turn effect by itself.

The shared start/end boundary is decisive: HMD under-response and thumbstick
coast are two manifestations of the same camera-follow/cancellation mismatch,
not independent controls bugs.

## Conclusive diagnostic result

The continuous capture spanning pickup, one death/reload after pickup, the
carry, and release is archived at:

`Builds/defense-continuous-post-scene-trace-20260906/defense_full_interval_trace.csv`

During ordinary play, Metro applies the synthetic mouse camera correction on
the expected short delay. During the child-carry state, the same yaw command is
smoothed out over approximately 15–20 rendered frames. The existing renderer
subtracts the booked synthetic mouse rotation on a fixed short delay. It
therefore subtracts rotation before Metro has actually applied it:

- physical HMD yaw is partially cancelled, making the world follow the head;
- Metro's remaining delayed response arrives later, producing thumbstick coast;
- the final rendered head response was commonly around one half during the
  affected interval.

The run containing a death/reload remained useful: it still captured both the
affected carry state and the immediate recovery when the child left the back.

## Approaches tried and what they established

Several early hypotheses were rejected by headset A/B tests:

- disabling shake damping, including pitch-only variants: no fix;
- changing/rebasing scripted-camera handoff and release: no fix, and some
  versions broke forced-look direction;
- standing down yaw follow during scripts: did not cure hitching and corrupted
  the permanent yaw bookkeeping after authored turns;
- clearing stale turn credit, post-release cooldowns, and yaw response/release
  rebases: no fix;
- bypassing or suppressing camera follow during/after the carry: caused the
  already-known culling, interaction, or scripted-scene regressions;
- forcing a native mouse response curve: no fix;
- control-state, native-state, and input-handoff classifiers: did not produce a
  reliable unique `child on back` flag suitable for a level-specific patch.

The useful progression was the full interval trace. It compared the same
session before pickup, during carry, and after release and exposed the long yaw
response tail directly.

## Last candidate: measured rendered-yaw cancellation

The final experiment stopped trusting commanded mouse units for rendering. It
maintained an explicit intended body heading from VR thumbstick turn requests,
measured Metro's actual engine-camera yaw, and treated the difference as the
render cancellation. A load-jump re-anchor was added after a video showed the
first version could load with the player facing roughly the wrong direction.

Artifacts:

- `Builds/defense-measured-render-yaw-fix-20260906`
- `Builds/defense-measured-render-yaw-load-reanchor-20260906`
- last deployed experimental SHA-256 before rollback:
  `7351189542575847AD3C232CCE0BB37192D48718076CE9659062CBFA375FE370`
- regression video: `<PRIVATE_VIDEO>/2026-09-06 01-38-40.mp4`

Result:

- fixed the original Defense HMD yaw and smooth-turn responsiveness/coast;
- the load re-anchor mostly fixed the wrong-facing/wrong-walking regression;
- walking still did not feel perfectly straight;
- the laser sight began moving slightly with the HMD;
- the change applied globally, despite the proven problem being limited to the
  child-carry interval;
- performance varied considerably between runs, so no experimental Defense DLL
  displaced the exact independently validated 72 FPS baseline.

Why the regressions make sense: the experiment created a new rendered body-yaw
authority without changing every consumer of body direction. Movement, laser,
weapon, shot, UI, and camera code could therefore disagree about which heading
was authoritative. It was evidence for the root cause, but not a safe release
architecture.

## Performance and diagnostic cautions

The game/mod showed inconsistent smoothness across repeated Defense runs. Some
diagnostic and candidate builds were exceptionally smooth; others hitched even
after diagnostics were removed. The user also observed hitching in the intro,
main menu background animation, scripted scenes, and ordinary gameplay, so not
all observed hitching was necessarily Defense-specific or caused by the mod.

There is nevertheless one proven performance invariant: sending pitch and yaw
as separate synthetic mouse inputs duplicated the synchronous cursor pinning
round trip and reduced performance. Combining both axes into one input restored
the validated smooth result. Do not split `SendLookAxes` back into separate
pitch/yaw delivery. Keep reverse-engineering diagnostics, GPU readbacks, periodic
camera traces, and per-frame file logging disabled in play builds.

The exact `4AD471D3...` DLL is the performance reference. A newly rebuilt DLL,
even from apparently equivalent source, must not be called equally smooth until
it passes an in-headset comparison.

## Mouse input, XInput, and a possible future architecture

The visible HMD pose is already applied directly in
`HackerContext::PatchMappedVRCameraData`. Synthetic mouse input exists to steer
Metro's internal camera so CPU culling, interaction, weapon effects, and related
systems follow the rendered view. The architectural problem is the delivery
mechanism, not OpenXR/OpenVR head tracking itself.

Mouse input is a confirmed contributor to keyboard/controller prompt switching,
menu cursor movement, scripted-camera contention, approximate response scaling,
and lag compensation. An older test moved both follow axes to virtual XInput and
still saw prompt switching, so mouse was not the only trigger in that build.
Since the later tilt/follow changes substantially reduced prompt switching, it
is plausible—but unproven—that removing the remaining mouse events now could
eliminate it. Retest rather than inheriting either old conclusion as absolute.

The 2026-08-19 XInput right-stick follow experiment is documented in the status
log around §5.16:

- pitch follow through XInput worked by itself;
- yaw produced jitter, snapping to an earlier heading, and inaccurate shooting;
- the failure came from bookkeeping: stick deflection is a turn rate, not an
  exact angular delta, and measured engine yaw contains both the mod's follow
  and the player's real body turn. Crediting it all to the mod corrupted
  `sCullInjectedYaw`;
- this does not prove XInput cannot turn Metro's camera, but it does make XInput
  an unattractive final architecture because it shares a rate-based game input
  channel with player turning.

The preferred future experiment is an isolated branch that locates and modifies
the state from which Metro constructs its camera **before culling and command
recording**. The known setter at `metro.exe+0x7E09EC` is too late by itself: it
replays a camera matrix after culling has already happened. Earlier upstream
orientation/player-state scans found derived outputs rather than authoritative
writable state, so finding the true construction point remains reverse-
engineering work.

If an upstream hook succeeds, expected consequences are favorable:

- culling and interaction should inherit the correct camera automatically;
- mouse-response prediction, fixed-lag cancellation, cursor pinning, and much
  scripted ownership inference can potentially be removed;
- shooting must be deliberately revalidated because `RedirectShot` currently
  consumes Metro's camera forward vector and removes injected-yaw bookkeeping;
- smooth/body turning, laser and weapon transforms, UI correction, scopes,
  crouch/lean/ladder states, level loads, and authored scenes need validation;
- a late final-matrix patch is not an acceptable substitute because it changes
  the picture without changing the culling camera.

This future work is safely disposable if performed on a new branch with the
current exact DLL retained as the installed fallback. Do not replace the game
DLL until a candidate is intentionally ready for headset testing.

## Recommended restart point

1. Branch from the restored baseline; do not revive all Defense candidates.
2. Preserve `4AD471D3...` as the instant fallback.
3. Trace the camera construction/recorder earlier than `+0x7E09EC` using a
   small, unmistakable rotation probe.
4. Before removing mouse follow, prove in one run that rendered direction,
   Metro's camera forward vector, culling, and interaction direction all change
   together.
5. Then remove old cancellation paths incrementally and validate shooting,
   laser, movement direction, loads, scripted scenes, and Defense child carry.

## Separate Defense watch-timer transition issue

Reported after the camera work was parked: playing continuously through the
child-pickup scene moves the custom time/filter timer off the physical watch and
leaves the native timer on the HMD. Loading any save made after that scene
restores the correct wrist display.

The affected runtime log showed the timer using its iconless atlas fallback
through the scripted interval. That fallback formerly accepted the first five
generic six-index glyphs on a previously validated timer atlas each frame. The
same log contains many unrelated generic-text matrices, so a new scripted HUD
block can consume the five slots before the real timer is submitted. The watch
geometry route independently accepted any 6,108-index draw as the primary
parent despite the casing's exact stable shader identity being known.

Candidate `Builds/defense-watch-timer-geometry-anchor-20260906` tightens both
boundaries. Only the 6,108-index draw with VS `79BA9F1ECFAF0CA1` and PS
`3B3535E6C07D32BA` can establish the Spartan casing parent. That exact physical
draw also resets and authorizes the iconless five-glyph fallback for its frame,
discarding unrelated same-atlas text encountered earlier. No diagnostic path or
camera change is enabled. Candidate/installed SHA-256 is
`116E342EA494BE8DE87A4A274F76B14699B33A1B7DBBE2F632C808AD4107EC59`; its
archive contains the exact smooth `4AD471D3...` fallback. Continuous scene
validation is pending.

That first candidate was rejected immediately in the headset: the physical
watch remained on Metro's native right hand, the custom timer vanished, and the
native HMD timer still appeared after the scene. Its archived run log captured
the real Defense 6,108 casing as VS `79BA9F1ECFAF0CA1`, PS
`6597099FD3B895DB`. Therefore the older `3B3535E6C07D32BA` material is not a
chapter-stable casing discriminator. Do not restore that exact-pair gate.

The follow-up candidate is
`Builds/defense-watch-timer-routed-casing-boundary-20260906`. It restores the
original broad physical-parent route exactly. When that existing route
successfully accepts a 6,108 casing, the event is used only as a same-frame
boundary for the iconless glyph fallback: it clears earlier same-atlas text and
allows the next five validated glyphs. It cannot reject or reposition the
physical watch. Candidate/installed SHA-256 is
`BE3C4EECBA0A6A4760B9A38D33B2B62BD9C4963819FB25D8C98D2AE7699968C6`;
continuous scene validation is pending.

Headset validation rejected the follow-up too. Correction to the initial user
message: the physical watch remained correctly attached to the **left** hand;
only the custom timer was missing, while the native timer remained on the HMD.
The run also hitched severely, so the exact `4AD471D3...` DLL was restored.

The preserved log shows the physical 6,108 casing reparenting successfully
after scripted-camera release. The scene spans about frames 2,730–6,496, which
expires `DrawWatchDisplay`'s last decoded digits after its 180-frame limit.
Although `timer-atlas fallback active` resumes afterward, no valid decode is
committed. This proves unrelated glyphs exist even after the casing boundary
and still consume the five fallback slots. The timer problem is classification,
not physical-watch parenting.

Do not add another order-only suppression predicate. A future diagnostic should
capture a tightly bounded set of same-atlas six-index candidates after the live
scene and after loading the same post-scene save, including frame/ordinal,
source `m_screen`, texture signature, and asynchronously decoded glyph. The
contrast should reveal the actual timer batch without high-frequency logging or
synchronous readback. The routed-casing change is removed from active source.

### Bounded post-scene timer candidate census

The next diagnostic implements the first half of that comparison without
changing any rendered behavior. Once a previously valid watch decode has been
stale for more than 600 frames, a fresh left-hand watch parent is present, and
the validated glyph atlas is bound, it copies at most 32 six-index quads from a
single frame into one staging buffer. Readback is delayed by at least two frames
and uses `D3D11_MAP_FLAG_DO_NOT_WAIT`; it never stalls waiting for the GPU. The
log is written once after capture and contains candidate ordinal, decoded glyph,
UV origin, texture hash/dimensions, and the source UI matrix rows needed to
separate the real timer batch from unrelated text. After that one census the
diagnostic disables itself for the remainder of the process.

This first run must be continuous from a working pre-scene watch through the
child-pickup performance to the broken post-scene state. Do not infer a fix from
this build: it is evidence-only. Preserve its log, inspect the broken-state
candidate set, then either identify a stable classifier directly or run a
separate working checkpoint-load census for comparison. Remove the diagnostic
once the classifier is known. Diagnostic/installed SHA-256 is
`D78F741871DDCA09DC2AB94BF47A6561A6E57658C85180C7133B6A0F741F23D1`.

The census succeeded at frame 7,147 with 13 candidates. Candidates 0–7 were
unrelated, non-decoding HUD glyphs on texture `76d55570`. Candidates 8–12 were
the coherent real clock batch on texture `35e3c17c`: four decoded digits
`1 0 4 9` followed by punctuation, with a shared matrix basis and sequential
positions. The user also saw the custom `10:49` flash on the wrist for an
instant while Metro's native timer remained continuously on the HMD.

This exposed a flaw in atlas learning, not a Defense-only resource change. An
icon-anchored five-draw sample that successfully decoded four digits committed
the texture signature from *every* captured draw. A preceding unrelated glyph
therefore caused both `35e3c17c` and `76d55570` to become validated. Once the
long scene removed the icon anchor and expired the cached value, the eight
earlier `76d55570` draws consumed the global five-glyph fallback window before
the real `35e3c17c` clock arrived.

The candidate fix commits a texture signature only when that draw's own UVs
decoded as one of the four clock digits. On a fresh process only `35e3c17c`
should be learned from this observed sequence; the unrelated texture can no
longer arm or consume the iconless fallback. This uses learned evidence rather
than a hard-coded texture hash and remains applicable across chapters and
resource recreations. The census staging buffer, polling, capture calls, and
logging were removed completely. Candidate archive:
`Builds/defense-watch-timer-decoded-texture-learning-20260906`. Candidate and
installed SHA-256:
`3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985`.

Headset validation passed in the required continuous before-scene through
post-scene run. The custom time/timer remained on the left wrist and the native
HMD timer no longer persisted. The successful log confirms that a fresh process
validated only `35e3c17c` (`set=1`); the false `76d55570` atlas was not learned,
while the iconless fallback continued operating throughout the long scene.
Preserved log:
`Builds/defense-watch-timer-decoded-texture-learning-20260906/d3d11.validated-headset-run.log`.
This fix is accepted and remains installed. No Defense census diagnostic remains
active.

## Defense child-pickup scripted hands

After the timer fix, the remaining Defense-specific visual problem is the
pickup performance: ordinary gameplay hands are controller-attached, but the
authored scene needs Metro's complete native arms/hands so the hands remain
connected to the arms. The hands currently do not appear during that scene.
The player correctly noted that this occurs after Armory's clothing change,
which replaces the ordinary 12,132-index hand mesh with the 21,762-index
post-clothing family.

The existing scripted hand component already recognizes the 12,132, 21,762 and
reused 21,252 opening/prologue arm families and preserves them natively during a
positively identified hand performance. The successful timer-validation log,
however, shows the 21,252 family routed to controller attachment near frame 715
with no native ownership transition. The exact native viewmodel effector only
asserted much later for frames 9,878–9,904. This strongly suggests that the
pickup performance does not assert either player-state pattern previously
validated in Riga and Armory, rather than simply using an unknown hand mesh.

An evidence-only diagnostic records changes to raw player fields `+0xF78`,
`+0xF90`, `+0x15D0`, `+0x15D2`, and `+0x1850`, capped at 128 lines for the
process. It also logs only route-state changes for the opening and combined
normal/post-clothing hand families, capped at 32 and 64 lines respectively.
There is no GPU readback, polling loop, render suppression, or behavioral
change. Only the initial pickup scene is needed. Archive:
`Builds/defense-scripted-hands-state-diagnostic-20260906`; diagnostic/installed
SHA-256:
`F2257455586238B804B3CA6D092A9AEA5F7ED57234F3E7E8202689112D28F5C9`.
Its pre-change DLL is the validated timer fix `3EAB9B24...`.

The headset diagnostic resolved the issue. Defense does use the already
validated player-performance signal: `(playerMode, performanceMode)` changes
from `(0,0)` to `(1,1)` at frame 2,793, stays asserted through the pickup
performance, and releases at frame 7,184. The post-clothing 21,762 draw enters
native scripted routing for that entire interval. Therefore no new state signal
or level-specific detector is needed.

The actual bug was family priority. Metro submits both the post-clothing hands
and the reused complete 21,252 arms/hands mesh. The renderer always treated the
post-clothing mesh as authoritative and discarded the 21,252 mesh before its
scripted-preservation branch, even though only the latter contains the authored
complete arms for this scene. In ordinary gameplay that priority prevents a
duplicate reused hand and is correct; during a positive scripted performance it
removes the geometry the scene needs.

The candidate reverses priority only while `ScriptedHandPerformanceActive()` is
true: it preserves the complete 21,252 native arm/hand draw and suppresses the
redundant 21,762 post-clothing hand draw while that complete family is current.
Outside the scripted interval, the existing post-clothing controller-attached
hand remains authoritative exactly as before. All raw-state and route diagnostic
logging was removed. Archive:
`Builds/defense-scripted-complete-arms-priority-20260906`; candidate/installed
SHA-256:
`A1DE597F0D370DC69D14706EBD4BA72839EBAD59939B6790557CA4F427135D62`.
The archive preserves the diagnostic evidence and clean timer-fix fallback.

Headset testing rejected that priority candidate: the hands remained absent.
This proves the earlier 21,252 observation did not establish that the mesh was
still submitted during the actual `(1,1)` pickup interval. The functional
priority change was removed completely; do not infer a scripted mesh lifetime
from a draw observed earlier in the level.

The next behavior-neutral census runs only while the already validated scripted
hand state is active. It records each unique non-instanced viewmodel draw and
each unique instanced draw using either the viewmodel pass or matrix-instance
layout, including index/instance counts, start/base locations, shader pair,
layout/pass classification and pending instance-buffer state. Each category is
capped at 256 unique signatures for the process; there is no GPU readback and
no rendering change. Archive:
`Builds/defense-scripted-geometry-census-20260906`; diagnostic/installed SHA-256:
`01B54F9E450DC7DBBF954170320665C6E94BB9E60C4A8E37D82D8F1C98283925`.
Its fallback is the clean validated timer fix `3EAB9B24...`.

The headset census confirms that the scripted interval continuously submits
the 21,762 post-clothing mesh and that the existing renderer routes it through
the native scripted path. The only other `IsViewmodelPass()` meshes are the
weapon body (`10,875`), ammunition loops (`5,292`), canisters (`4,176`), and
the already-known watch/detail passes. Offline post-VS renders corrected an
initial mistaken suspicion that the first three were arms: they are weapon
components. The 21,252 opening/prologue complete-arm family is not submitted
during this Defense performance. Do not redirect any of those three weapon
families as a hand fix.

This reduces the unresolved problem from identity/state/routing to the pose of
the correct native mesh. The broad census was removed and replaced with a
two-sample capture on the exact 21,762 draw: once before positive scripted
ownership and once after it begins. Each sample reads only the 64-byte native
instance matrix and the bound b8 bone palette, writes
`defense_hands_{before,scripted}_{instance,palette_b8}.bin`, and disarms for
the process. There is no render change and no repeated GPU readback. Archive:
`Builds/defense-scripted-hand-pose-capture-20260906`; diagnostic/installed
SHA-256:
`D5064B6EFF654886DEA4B5F86E90210D72D99350A9449CE2CEDD65A08A7C8E11`.
The archive preserves the complete census run and its exact pre-change DLL.

Both requested pose captures succeeded. Offline skinning of every connected
component shows that Defense supplies a coherent authored animation: before
the scene the mirrored hands occupy the ordinary separated gameplay pose; in
the first scripted sample both hands and all cuff details move together into a
compact pose roughly 0.35–0.65 m below/in front of the native origin. Nothing
is collapsed, zero-scaled, NaN, or displaced to a distant world coordinate.
The instance also remains a valid near-camera rigid transform. Therefore the
game's authored skeleton and native instance are not the cause of the missing
pixels.

The remaining renderer-state difference is the viewmodel scissor. The ordinary
controller-hand path already clones the rasterizer with scissoring disabled,
with an existing code comment documenting that Metro's rectangle can erase
both combined hands at the lower headset edge. The native scripted path did
not use that protection, and Defense places both hands precisely in that lower
region. The candidate disables scissoring only for recognized 12,132/21,762
hand draws during positive scripted-hand ownership, reapplies it after twin-eye
setup, and restores the exact prior raster state afterward. Gun, watch, world,
ordinary gameplay, and generic native-effector draws are unchanged. All pose
capture/readback code was removed. Archive:
`Builds/defense-scripted-hands-no-scissor-20260906`; candidate/installed SHA-256:
`6E9F21FCEFE78B5B3BF8CCF769EF3C0901052F413E0E100994ED85C4FC5B4CE3`.

Headset testing rejected the no-scissor candidate; the hands remained absent.
The scripted-hands issue is minor and has been parked at the user's request.
All behavior and diagnostic code from this investigation is dormant/removed,
and both source and the installed game DLL were returned to the exact validated
Defense timer-fix checkpoint. Restored SHA-256:
`3EAB9B24D7844B0AA8EFCBA228ABDAFDAE66C947AB33FCDE464098530798C985`.
Retain the archived logs, pose buffers, and failed candidates for a future
investigation, but do not resume them without a new explicit request.

## Final release audit

The final cleanup compared the tracked renderer against commit `db34711`, the
recorded validated timer checkpoint: there is no source delta in
`HackerContext.cpp` or `VRPose.cpp`. The installed DLL and archived validated
DLL are byte-identical at SHA-256 `3EAB9B24...`. The working implementation is
compiled into the proxy: custom watch rendering and native-HMD suppression are
enabled constants, while the corrected decoded-glyph-only atlas validation is
part of the renderer itself. It does not load a Defense patch, capture, CSV,
texture list, or other external diagnostic artifact. Runtime atlas learning is
intentional resolution/quality-independent behavior, not a diagnostic.

A source audit found no enabled Defense diagnostic/census/pose-capture switch
and no `defensehands` hook. Removed the six leftover runtime evidence files
from the game directory: the two camera CSV traces and four hand pose buffers.
Identical evidence remains recoverable in the corresponding ignored `Builds/`
archives. No accepted behavior was removed.
