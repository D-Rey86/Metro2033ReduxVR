# SteamVR refused every frame because the game uses DXGI 1.0 — 2026-07-30

## Symptom

With milestone 2's compositor submission in place (Notes/11 step 1),
`IVRCompositor::Submit` failed on every frame with
`VRCompositorError_SharedTexturesNotSupported` (106), while everything
around it looked healthy:

- `VR_Init(VRApplication_Scene)` succeeded, pose tracking worked.
- `CanRenderScene()` returned 1.
- The game and SteamVR were confirmed on the *same* GPU - logged both
  LUIDs: `NVIDIA GeForce RTX 5070 Ti (LUID 00000000:0000f14b)` for the
  game, and `GetDXGIOutputInfo` reported adapter index 0 which resolved
  to the identical LUID. Cross-adapter sharing ruled out.
- Submitting our private texture with `D3D11_RESOURCE_MISC_SHARED`,
  with `MiscFlags = 0`, and submitting the swap chain's back buffer
  directly all produced the identical error.

That last point was the important one: if the game's *own* back buffer
is equally unacceptable, the problem is not the texture.

## Root cause (from SteamVR's own log, not inference)

`C:\Program Files (x86)\Steam\logs\vrclient_metro.txt`:

```
[Info]  - Initializing the limited version of CVRCompositorClient
...
[Error] - Failed to create sync texture.  Ensure application was built
          using DXGI 1.1 or later (i.e. Call CreateDXGIFactory1).
```

Metro 2033 Redux (2014) creates its DXGI factory through the legacy
DXGI 1.0 entry point - confirmed in our own log:
`*** Hooked_CreateDXGIFactory called with riid: IDXGIFactory`. SteamVR
needs DXGI 1.1+ to create the shared "sync texture" its full compositor
client depends on. Failing that, it initialises a *limited* compositor
client, which cannot accept shared textures at all - so every `Submit`
returns 106 regardless of what is submitted or how it was created.

Worth recording as a process lesson: several rounds were spent
theorising about texture flags, sharing modes and adapters. The
answer was sitting in the refusing component's own log the entire
time. Read the other side's logs earlier.

## Fix

`Hooked_CreateDXGIFactory` (`HookedDXGI.cpp`) now services the game's
DXGI 1.0 request by actually calling `CreateDXGIFactory1`. This is safe
and transparent:

- `IDXGIFactory1` derives from `IDXGIFactory`.
- `CreateDXGIFactory1` accepts `__uuidof(IDXGIFactory)`, so the game
  receives exactly the interface it requested.
- On any failure it falls back to the original `CreateDXGIFactory`, so
  a system where this doesn't work degrades to previous behaviour
  rather than failing to launch.

3Dmigoto already upcast the returned factory *interface* to
`IDXGIFactory2` via `QueryInterface`, but that doesn't help here - what
SteamVR objects to is the entry point used to create it, which
determines the process's DXGI mode.

Confirmed applying in the log (all three of the game's factory calls):
`Metro2033ReduxVR: upgraded CreateDXGIFactory -> CreateDXGIFactory1 for
SteamVR compositor support`.

## Related fix: don't let a broken compositor hang the game

While submission was failing, the game appeared to hang with SteamVR
showing its "waiting for application" room. Cause: `WaitGetPoses`
blocks by design to pace the app against the compositor, which is
correct while frames are being accepted but leaves the game throttled
against a compositor that will never display anything when they aren't.

`SubmitFrameToCompositor` now counts consecutive submission failures
and abandons the compositor path entirely after 200 of them, logging
why and reverting to milestone-1 behaviour (head tracking, flat
output). Degraded but responsive beats frozen.

## Also relevant: the OpenXR runtime setting

Virtual Desktop was configured with **VDXR** (its own OpenXR runtime)
rather than SteamVR. Earlier in the project the user asked whether that
setting mattered and was told it didn't - true at the time, since
milestone 1 only *read* poses (which work through VD's SteamVR driver
regardless), but it stopped being true the moment we tried to submit
frames, and that wasn't revisited.

With VDXR active, SteamVR never engaged at all. Switching VD's runtime
to SteamVR got SteamVR to launch and take the app - which is what
finally surfaced the real DXGI error above. Both changes were needed:
the runtime setting to get SteamVR involved, and the DXGI upgrade to
make it accept our frames.

**Open question for the future**: targeting OpenXR/VDXR directly may
be the better long-term path for this hardware (Galaxy XR over Virtual
Desktop) - fewer layers, and it's what VD recommends for performance.
The pose/submit concepts map closely onto what's already built, so it
is less work than it sounds. Worth revisiting if the SteamVR path
proves awkward.

## Status

DXGI upgrade built, deployed, confirmed applying in the log. **Not yet
confirmed to fix submission** - needs a headset test with VD's runtime
set to SteamVR. Expect either a
`VRPose: first successful frame submission to OpenVR compositor` line,
or a different (and more informative) error now that the compositor
client should no longer be the limited one.
