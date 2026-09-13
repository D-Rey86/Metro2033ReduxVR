# Separated left hand and wrist watch — complete implementation handoff

Last verified: 2026-08-23

Branch: `codex/left-hand-controller-attachment`

Verified commits: `8d655c8`, `d52a9e4`

Verified DLL SHA-256: `9844431782D97D87D5F26BA8246F3CF0843EF61B2F09A0AA7D2122B95B66CEF2`

Current embedded-default build SHA-256: `4593D0D518746AECB965F3D9EC8DE7DAB014159E0B4826135DC15BE895A3F386`

This is the authoritative handoff for the work that separated Metro's left
hand from the gun/right hand, attached it to the physical left controller,
attached every watch component to that hand, rebuilt the digital display in
stereo, and made one preferred left-hand pose persist across weapons. Read this
before changing any hand or watch code. The chronological status log contains
the individual experiments; this file records the final architecture and the
reasons behind it.

## Verified player-facing result

- The right hand and gun remain on the right controller through the existing
  weapon instance path.
- The left-hand triangle island receives its own rigid instance driven by the
  left controller.
- Physical movement and rotation are one-to-one enough to remove the original
  head-relative drift and large swinging/orbiting behavior.
- The headset-selected left-hand pose is embedded in the DLL and remains
  unchanged when switching weapons. A valid `vr_left_hand_pose.bin` can
  intentionally override it.
- The full watch casing, orange hardware, cyan darkness indicator, and custom
  time/filter display move as one assembly on the left wrist.
- The watch does not inherit Metro's walking bob.
- The custom display uses Metro's actual native digits as its data source. It
  shows local real-world time normally and changes to Metro's filter countdown
  when a filter is active.
- Timer updates are initialized promptly and interpolate naturally through
  decade boundaries such as `4:50 -> 4:49`, rather than briefly showing
  `4:40`.
- The custom display is rendered independently for both eyes and uses depth
  testing, so it does not show through the wrist.
- Chapters that omit the watch-icon draw no longer leave the native timer on
  the HMD; the fallback is learned only from a validated timer batch.

## Primary source locations

Most rendering logic is in
`ThirdParty/3Dmigoto/DirectX11/HackerContext.cpp`:

- `IsWatchIconDraw`, `UpdateWatchBlockTracking`, `IsWatchDigitDraw`
- `CaptureWatchDigit`, `MatchWatchDigit`
- `DrawWatchDisplay`, `BuildWatchDisplayAffine`
- `BeginLeftHandBonePalette`, `EndLeftHandBonePalette`
- `SubstituteLeftHandCompletedInstance`
- `DrawIndexedInstanced`, especially the 12132-index split and physical-watch
  routing immediately before it
- Global completed-hand/watch matrices near `sLeftHandCompletedInstance`

Persistent offsets, calibration input, and controller/head transforms are in
`ThirdParty/3Dmigoto/DirectX11/VRPose.cpp`:

- `LoadLeftHandAdjust`, `SaveLeftHandAdjust`
- `LoadWatchAdjust`, `SaveWatchAdjust`
- `LoadWatchTimeAdjust`, `SaveWatchTimeAdjust`
- `GetLeftHandRotationDelta`, `GetLeftHandTranslationDelta`
- `GetLeftHandControllerRelativeToHead`
- `GetLeftHandControllerRotationRelativeToHead`
- `GetWatchAssemblyCalibration`, `GetWatchTimeCalibration`

Declarations are in `HackerContext.h` and `VRPose.h`.

Analysis scripts retained for reproducing the mesh measurements:

- `Tools/analyze_hand_mesh.py`
- `Tools/analyze_watch_mesh_components.py`
- `Tools/dump_watch_glyph_uvs.py`
- `Tools/save_watch_glyph_atlas.py`

## Proven hand draw identity and geometry split

Metro submits both first-person hands in one skinned draw:

- `IndexCountPerInstance = 12132`
- one combined mesh containing two disconnected islands
- bone palette at vertex constant-buffer slot `b8`
- palette size: 3856 bytes = 241 `float4` rows

The exact contiguous index split is:

| Range | Index count | Meaning |
| --- | ---: | --- |
| `StartIndexLocation + 0` | 1443 | first right-hand range |
| `StartIndexLocation + 1443` | 6066 | complete left-hand island |
| `StartIndexLocation + 7509` | `12132 - 7509` = 4623 | second right-hand range |

The current code draws the two right ranges through the normal weapon/right
controller instance and draws only the middle range through the left-hand
instance. Do not attempt to separate the hands by shader hash or by making two
copies of the complete draw; the contiguous island split is exact and already
verified in the headset.

The left-hand vertices use these 18 three-row affine bone entries:

`0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51`

Each base refers to matrix rows `base + 1` through `base + 3`. The right hand
must continue receiving Metro's untouched live palette.

### Weapon-switch diagnostic result

A diagnostic sampled the actual resources while switching several weapons.
Every tested weapon used the same hand geometry and material:

- VB0 hash `8ee4bf35`, stride 32, offset 0
- VB1 hash `19054e4d`, stride 64, offset 64
- IB hash `b25e5965`
- PS slot-0 material hash `91078c12`, 2048x2048
- VS `79BA9F1ECFAF0CA1`
- PS `32B3ADE9DE302235`

Only the bone palette changed. This is why the preferred-hand implementation
saves bone rows rather than retaining vertex/index buffers or replaying another
weapon's draw resources.

## Left-controller transform architecture

The working implementation does not apply a controller delta around an
arbitrary model pivot. It constructs a desired rigid hand instance in
controller/head space and places an anatomical wrist landmark at the physical
controller plus the user's calibrated local offset.

Important properties:

1. `BeginLeftHandBonePalette(true)` reads Metro's current `b8` palette into a
   private CPU vector. The game's dynamic buffer is never modified.
2. Weighted coefficients measured from the captured mesh evaluate the skinned
   wrist, overall hand center, and fingertip directly from the palette without
   reading the vertex buffer every frame.
3. Controller rotation is converted into a right-handed orthonormal basis.
   This fixed the earlier flattening/warping caused by a derived third axis with
   changing length, non-perpendicular axes, and a determinant that changed
   sign.
4. The controller-local authored basis is captured once. User pitch, roll, and
   wrist twist are applied in that local frame, then the current absolute left
   controller rotation is applied.
5. Translation is solved so the evaluated wrist lands exactly at the
   controller plus `vr_left_hand_offset.txt`.
6. The viewmodel's view correction is pre-cancelled before the rigid instance
   is submitted. This follows the same invariant as the proven gun path and
   prevents head movement from being counted twice.
7. Eye 0's completed rigid instance is reused unchanged for eye 1. Recomputing
   or independently pre-cancelling eye 1 removes the relative-eye transform and
   caused a large stereo mismatch in testing.
8. Scissoring is disabled only for the hand draw; Metro sometimes binds a
   viewmodel/UI scissor that clips the hands at the headset edge.
9. Hands are suppressed when `VRPose::IsGameplayModeActive()` is false. Without
   that gate, absolute controller placement makes them visible during title and
   intro screens where Metro normally hides them by transform.

The original problem where the hand swung instead of rotating came from
applying a left-controller bone delta inside an instance still driven by the
right-hand/gun transform. The final approach gives the isolated triangle range
its own completed rigid instance, so there is no right-controller parent left
to create an orbit.

## Embedded preferred hand pose and optional override

The exact headset-selected pose is compiled into `HackerContext.cpp` as
`kEmbeddedPreferredLeftBoneRows`. Its 864-byte float payload matches the
original selected `vr_left_hand_pose.bin` bit-for-bit. Payload SHA-256:

`3E6018D6344146070E784354B92BAA519A0ABC382812DE785F3484142C381B3A`

This embedded array is the distribution default. Every player receives the
same selected hand even if the package contains only the DLL and no pose file.

The game-directory `vr_left_hand_pose.bin` remains an optional advanced-user or
developer override.

File layout:

- 4-byte little-endian magic `0x4C485031` (`LHP1`)
- 18 entries x 12 floats x 4 bytes = 864 bytes
- total size 868 bytes

At startup, `BeginLeftHandBonePalette` copies the embedded rows into the active
preferred-pose array. If a valid external file with the header and payload above
exists, it replaces the embedded rows. A missing or invalid file retains the
embedded pose. There is no automatic first-equipped-weapon capture anymore;
deleting the external file deliberately restores the distribution default.

The saved rows are substituted only in the private palette. The preferred
palette is bound for the isolated left range in eye 0, restored before entering
the twin pass, then bound and restored explicitly around eye 1. Never begin a
twin pass while the private hand palette is still the active baseline; that
allowed a hand-only `b8` state to leak into later skinned draws.

### Critical watch separation

Keep two palette views:

- `palette`: preferred/frozen rows for the visible hand and its wrist placement
- `livePosePalette`: Metro's untouched current rows for the watch-parent wrist
  reference

This distinction is essential. An intermediate build evaluated watch placement
from the frozen hand landmark while drawing/calibrating against the older live
hierarchy. The hand worked, but the watch was routed far away and appeared
missing. The verified build keeps the visible hand frozen while preserving the
watch's original live-wrist reference.

## Saved hand calibration

`vr_left_hand_offset.txt` contains six whitespace-separated floats:

`positionX positionY positionZ rotationPitch rotationYaw rotationTwist`

Current verified value:

`-0.018000 0.060000 -0.220000 0.999999 -1.049999 0.000000`

Development calibration controls (disabled in the release input path after the
final values were embedded):

- Hold LT + left grip for about one second to toggle left-hand adjustment.
- Left stick X: position X.
- Left stick Y: position Y.
- Left grip + left stick Y: depth/Z.
- Right stick Y: pitch.
- Right stick X: yaw.
- Right grip + right stick X: local wrist twist.
- Toggle the same chord off to save.

The right grip is a modifier; merely pressing it must not rotate the hand. The
right stick touch-state handling prevents stale OpenVR axis values from moving
the calibration after the stick is released.

## Physical watch draw identities

The watch is not one draw. The complete physical assembly consists of:

| Index count | Purpose / admission rule |
| ---: | --- |
| 6108 | primary casing and native hardware; establishes native watch instance |
| 2628 | physical child admitted only when its 12 affine values match the current-frame 6108 instance |
| 750 | first material pass, same exact-matrix guard |
| 750 | second material pass, same exact-matrix guard |
| 60 | conditional cyan darkness indicator; exact shader pair and same-matrix guard |

The cyan pass shader pair is:

- VS `65C62A5148831C58`
- PS `94ED7704592CAC87`

Its `StartIndexLocation` and `BaseVertexLocation` changed across levels, so they
must not be used as identity. The 60 count, shader pair, and live native-matrix
equality are the stable discriminators.

The earlier 1296-index candidate is a gun component, not the watch. Never route
1296 to the left wrist.

The physical child route is `SubstituteLeftHandCompletedInstance`. It uses a
fixed measured watch-local transform under the completed watch sibling, applies
one assembly calibration, updates the private instance buffer, and lets every
physical pass consume the same final transform. The watch can be submitted
before the hand in a frame, so the current or immediately previous completed
hand/watch parent is accepted; anything older is rejected to prevent stale
watch resurrection during loading transitions.

### Watch parent and bob removal

The visible hand and watch are sibling completed instances:

- Hand translation uses the current preferred-pose wrist landmark so the
  skinned wrist stays exactly on the controller.
- Watch translation uses a stable reference captured from Metro's untouched
  live wrist hierarchy.

This avoids two separate regressions:

- Parenting the watch directly to the hand's animation-compensated translation
  made it inherit inverse walking bob.
- Replacing the watch reference with the frozen preferred-hand landmark broke
  the already calibrated watch placement and moved the assembly out of view.

## Watch assembly calibration

`vr_watch_assembly_offset.txt` contains six floats:

`positionX positionY positionZ rotationX rotationY rotationZ`

Current verified value:

`-0.907504 0.204000 -0.456001 -0.110000 -0.020000 0.000000`

Controls:

- Hold LT + right grip for about one second to toggle whole-watch adjustment.
- Left stick X/Y: lateral and vertical position.
- Left grip + left stick Y: depth.
- Right stick Y/X: rotation X/Y.
- Left grip + right stick X: roll/Z.
- Toggle the chord off to save.

This one calibration moves the casing, orange pieces, cyan indicator, and time
parent together. Do not add separate placement corrections to individual
physical passes.

`vr_watch_offset.txt` is a legacy three-float calibration from the abandoned
controller reconstruction. It remains loadable for historical code, but it is
not the active assembly placement and must not be inserted into the current
parent hierarchy.

## Native time/filter source and custom 3D display

Metro's time is not reproduced from an independent clock or guessed filter
duration. The mod reads the native glyph stream, so all game logic remains
authoritative—including the automatic transition from local time to filter
countdown.

Relevant shader identities:

- watch icon VS `A9037683D2AF5BC0`
- watch icon PS `B468E0743796D214`
- generic text VS `7B3EB7275D556B14`
- generic text PS `4A48D5D4C49DCDF2`

The generic text pair is shared by menus and other HUD strings. Never suppress
or move every draw using it.

Normal classifier flow:

1. The unique watch-icon draw arms the classifier for the remainder of that
   frame.
2. Five six-index generic-text glyph draws are consumed: four numeric glyphs
   plus the separator. Metro submits the punctuation after the digits rather
   than visual left-to-right order.
3. On a sample frame, `CaptureWatchDigit` reads the four-vertex UVs from the
   glyph vertex buffer and `MatchWatchDigit` maps atlas cells to digits.
4. A complete four-digit batch is published atomically. Partial new digits are
   never combined with stale trailing digits.
5. The native quads are suppressed and `DrawWatchDisplay` renders a dedicated
   four-digit seven-segment mesh plus both colon dots.

Sampling is normally every 30 frames because synchronous staging readback on
every glyph every frame is unnecessary. The display retains the last valid
decode for up to 180 frames.

### Iconless chapter fallback

Some chapters submit the icon during initialization, then stop submitting it
while continuing to draw the HMD timer. The first fallback incorrectly learned
one texture from any icon-anchored generic glyph and suppressed unrelated text.

The verified fallback stores a resource signature only after the same
five-glyph batch successfully decodes as four digits plus separator. It retains
all validated texture hash/dimension signatures from that batch. On iconless
frames it accepts only six-index draws using those validated resources under
the generic-text shader, resets grouping each frame, and stops after five.

If this ever regresses, check for all three log categories:

- `capture glyph=... digit=... texture=...`
- `validated timer texture ...`
- `timer-atlas fallback active ...`

A fallback that activates without later `watch decode` messages is selecting
the wrong glyph sequence and must not be broadened blindly.

### Custom display rendering

`DrawWatchDisplay` reuses the mod's custom digit buffers but builds a compact
four-digit watch face. It:

- includes four digits and both `:` dots
- builds its affine from the exact final 6108 casing instance
- explicitly supplies distinct eye-0 and eye-1 projection matrices
- disables culling so the face remains visible from either side
- enables read-only `LESS_EQUAL` depth testing, preventing digits from showing
  through the wrist while avoiding depth writes
- saves and restores shaders, layout, vertex/index buffers, constant buffer,
  topology, raster state, and depth state

`vr_watch_time_offset.txt` contains seven floats:

`positionX positionY positionZ rotationX rotationY rotationZ scale`

Current verified value:

`-0.000318 0.000303 -0.002759 -0.001678 -0.029611 -0.014000 0.956011`

Controls:

- Hold LT + both grips for about one second to toggle time-only adjustment.
- Left stick X/Y: position X/Y.
- Left grip + left stick Y: depth/Z.
- Right stick X/Y: rotation Y/X.
- Right grip + right stick X: roll/Z.
- Right grip + right stick Y: scale.
- Toggle the chord off to save.

## Important failed approaches

- Moving the entire 12132 draw moves both hands; the hands are one mesh.
- Applying left-controller rotation in bone space while retaining the gun's
  instance makes the left hand orbit/swing.
- Building calibration in head/view space creates head-motion drift.
- Deriving a non-orthonormal controller basis flattens and warps the hand and
  creates stereo disagreement.
- Pre-cancelling each eye independently removes the relative-eye transform.
- Replacing the watch's native child matrix directly with the hand matrix
  erases its authored local transform and makes it orbit away.
- Routing index count 1296 attaches a gun component, not the watch.
- Applying the 6108 native-digit index split to the 2628/750 passes reads beyond
  those smaller draws.
- Matching generic text by shader alone breaks menus and unrelated UI.
- Learning a timer resource from every icon-anchored glyph selects unrelated
  text in some chapters.
- Using Metro's native flat time quads as a physical wrist surface caused eye
  mismatch, movement inside the casing, and a stubborn last-glyph mismatch.
- Freezing preferred bones only in wrist calculations while drawing live bones
  makes weapon switches progressively displace the hand.
- Leaving the private hand palette active while beginning the twin pass can
  leak hand-only skinning state into later viewmodel children.
- Driving the watch from the frozen preferred-hand landmark invalidates the
  calibrated live-wrist hierarchy and makes the watch appear missing.

## Future VR-menu option: separated versus original hands

The current compile-time switch is the local
`static const bool kSplitCombinedHands = true` in the 12132 draw path. Replacing
only that constant with a menu setting is not sufficient because the watch and
timer features depend on the separated-hand parent.

Recommended setting:

- Name: `separatedHands`
- UI text: `SEPARATED HANDS`
- Default: `true`, preserving the verified current behavior
- Config key in the VR settings file: `separated_hands=1`
- Runtime-safe toggle; no render-target recreation is required

Files to change:

1. `VRMenu.h`: add `bool separatedHands` to `VRMenu::Settings`.
2. `VRMenu.cpp`:
   - default it to true in `SetDefaults`
   - write `separated_hands` in `Save`
   - read it in `Initialize`
   - add a checkbox row in the Controls tab and update that tab's row count
   - update row indices for turning/snap-angle controls after insertion
3. `HackerContext.cpp`: show the checkbox in the VR menu overlay and use one
   helper such as `SeparatedHandsEnabled()` everywhere listed below.

When `separatedHands == true`, retain all current behavior:

- split the 12132 index ranges
- call `BeginLeftHandBonePalette(true)` for the left range
- load the embedded preferred pose and accept `vr_left_hand_pose.bin` only as
  an optional override
- publish completed left-hand/watch matrices
- reparent physical watch passes
- suppress/decode native watch UI
- render the custom 3D watch display

When `separatedHands == false`, restore the legacy presentation as one coherent
mode:

- do not enter the 12132 split block; let the existing general
  `SubstituteWeaponInstanceBuffer` path draw the complete combined hands with
  the gun/right-controller viewmodel exactly as before
- do not bind the preferred left-hand palette
- do not route 6108/2628/750/60 through
  `SubstituteLeftHandCompletedInstance`; let those native viewmodel children
  follow the original weapon path
- do not suppress the native watch `HH:MM` glyphs
- do not call `DrawWatchDisplay`
- do not hide native digit meshes or any native watch surface
- leave `vr_left_hand_pose.bin` and all calibration files untouched so toggling
  separated hands back on restores the user's setup immediately

The gates that currently reference compile-time `kUseCustomWatchDigits`,
`kHideNativeWatchUI`, or `kSplitCombinedHands` must use the runtime mode as
well. At minimum inspect:

- `UpdateWatchBlockTracking` call in `BeforeDraw`
- `iconAnchoredWatchDigit` and validated-resource fallback classification
- native glyph suppression/capture block
- `DrawWatchDisplay` call after the viewmodel pass
- physical `leftWatchCasing` route in `DrawIndexedInstanced`
- the 12132 split block itself

A safe pattern is:

```cpp
const bool separatedHands = VRMenu::GetSettings().separatedHands;
const bool customWristWatch = separatedHands && kUseCustomWatchDigits;
```

Use `customWristWatch` for decode/suppression/display and `separatedHands` for
the hand split and physical watch reparenting. Avoid clearing completed parent
matrices when toggling off; stale data is already guarded by frame age, and
preserving it allows a clean toggle back on. The first separated frame will
publish a fresh parent before the watch route is accepted.

Test matrix for the future option:

1. Start with separated hands on: left hand/controller, fixed pose, watch, cyan
   bar, time, and filter countdown all work.
2. Toggle off while holding a gun: both hands and native watch return to the
   original combined right-controller/viewmodel behavior; no custom digits
   remain floating on the HMD or wrist.
3. Switch several weapons while off.
4. Toggle on: saved preferred pose and all three calibrations return unchanged.
5. Verify both eyes and rotate the controller through a full range.
6. Walk to check hand/watch bob and drift.
7. Test a chapter with the icon and a chapter that requires the validated
   iconless timer fallback.
8. Test filter on/off transitions.
9. Open title, pause, equipment, weapon-selection, and merchant screens to
   ensure the generic-text classifier remains closed outside the exact timer.

## Recovery and verification

The known-good source checkpoint is `d52a9e4`. The preceding complete
left-hand/watch checkpoint is `8d655c8`. The final verified candidate is:

`Builds/persistent-hand-live-watch-parent-20260823/d3d11.dll`

Its SHA-256 is:

`9844431782D97D87D5F26BA8246F3CF0843EF61B2F09A0AA7D2122B95B66CEF2`

The distribution-ready build that embeds the exact same selected pose is:

`Builds/embedded-preferred-left-hand-pose-20260823/d3d11.dll`

Its SHA-256 is:

`4593D0D518746AECB965F3D9EC8DE7DAB014159E0B4826135DC15BE895A3F386`

The embedded 864-byte pose payload is byte-for-byte identical to the payload
in the user's verified `vr_left_hand_pose.bin`. Therefore distributors do not
need to ship that file. Keeping it beside the DLL is harmless and overrides
the built-in rows with identical data; deleting it exercises the distribution
default.

Always preserve these game-directory files before deploying an experimental
build:

- `vr_left_hand_offset.txt`
- `vr_left_hand_pose.bin`
- `vr_watch_assembly_offset.txt`
- `vr_watch_time_offset.txt`

Always verify source and deployed DLL SHA-256 hashes. The 3Dmigoto post-build
copy is gated to another username and does not deploy automatically.
