#pragma once

// Metro2033ReduxVR milestone 1: poll HMD orientation from OpenVR once per
// frame and cache it as a game-space row-major 3x3 DELTA rotation (relative
// to a captured reference orientation, not an absolute replacement - see
// UpdateVRPose) for HackerContext::PatchMappedVRCameraData to compose on
// top of the game's own view rotation. See Metro2033ReduxVR/Notes/ for the
// constant buffer layout this feeds into.
//
// Deliberately independent of whether a headset is present: if OpenVR
// isn't available (no SteamVR running), every call here is a safe no-op
// and G->vrHeadRotationValid stays false, leaving the game's rendering
// completely unmodified.

// Forward declarations so this header stays free of the (heavy) D3D11
// headers - every translation unit that uses these already includes
// d3d11_1.h via Globals.h.
struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;

namespace VRPose {

	// The frame number of the most recent confirmed shot (RedirectShot),
	// or 0xFFFFFFFF if none yet this session. Lets other translation
	// units narrow diagnostics to "did this happen right after a shot"
	// without duplicating the fire-hook's own bookkeeping.
	unsigned LastShotFrame();

	// The confirmed live magazine-ammo address from the heap hunt in
	// RedirectShot/RunAmmoHunt (see VRPose.cpp), or NULL if not yet
	// confirmed this session (needs a reload-to-known-value-then-fire
	// pass; see the ammo hunt's own comments for why it can't bootstrap
	// on its own). Heap address, not module-relative - stays valid only
	// for the lifetime of this process, unlike VRPose's other constants.
	void *ConfirmedAmmoAddress();

	// Same as ConfirmedAmmoAddress, for the reserve-ammo count (see
	// RunReserveHunt in VRPose.cpp - narrows by stability rather than by
	// decreasing, since reserve doesn't change on every shot).
	void *ConfirmedReserveAddress();
	bool GetReticleDirection(float out[3]);

	// Metro2033ReduxVR milestone 2: copies the just-rendered frame out
	// of the swap chain's back buffer and hands it to the OpenVR
	// compositor for both eyes, so the game is presented as a real VR
	// scene (compositor-timed, reprojected, filling the headset's FOV)
	// rather than as a flat mirrored desktop image. Must be called
	// BEFORE the real IDXGISwapChain::Present - after it, the back
	// buffer's contents are no longer guaranteed. Safe no-op when
	// OpenVR isn't initialised or the compositor is unavailable, so
	// flat-screen play is completely unaffected. See Notes/11.
	void SubmitFrameToCompositor(IDXGISwapChain *swapChain, ID3D11Device *device, ID3D11DeviceContext *context);
	// Converts Metro's completed two-slice sRGB scene target into independent
	// high-resolution UNORM eye textures without touching the game backbuffer.
	bool CaptureHighResolutionScene(ID3D11Texture2D *sceneTexture,
		ID3D11DeviceContext *context, unsigned frame);
	bool GetHighResolutionOverlayTarget(ID3D11RenderTargetView **leftView,
		unsigned *width, unsigned *height);
	// Returns the valid per-eye dimensions requested by the active OpenVR
	// runtime. These are the final submission dimensions, not Metro's internal
	// widescreen render canvas.
	bool GetRuntimeSubmissionSize(unsigned *width, unsigned *height);
	// Presents the mod settings UI as a compositor-owned, head-relative quad.
	// The menu texture is independent of Metro's stereo scene targets.
	bool PresentMenuOverlay(ID3D11Texture2D *texture);
	// Profiler HUD: its own head-relative overlay below the line of sight.
	// The texture is re-uploaded only when contentChanged.
	bool PresentPerfOverlay(ID3D11Texture2D *texture, bool contentChanged);
	void HidePerfOverlay();
	void NotifyFullscreenVideoDraw(unsigned frame);
	bool IsFullscreenVideoFrame(unsigned frame);
	// Cinema frames (after S.T.A.L.K.E.R. VR): loading screens with no 3D
	// scene are presented on one room-fixed screen for both eyes with their
	// 2D UI left native, instead of per-eye HUD placement where every 2D image
	// lands differently in each eye. UpdateCinemaFrame runs after Present and
	// decides for the next frame.
	void UpdateCinemaFrame();
	bool IsCinemaFrame();
	// A 3D scene pass (scene-sized colour target with depth) was bound in
	// `frame`. The loading-panel shader also draws the "press any button"
	// prompt over the live 3D fly-through; only a frame WITHOUT a 3D scene
	// is a real loading screen.
	void NotifySceneRendered(unsigned frame);
	// The deferred G-buffer fill (three scene-sized colour targets plus depth)
	// was bound in `frame`. Only a real, lit 3D scene does this; it is the
	// primary "3D scene present" signal for the screen-state rules.
	void NotifyGBufferRendered(unsigned frame);
	// Metro's front-end mode byte == 1 (main-menu level), read once per
	// Present in UpdateCinemaFrame - cheap for per-draw callers.
	bool IsNativeMainMenuCached();
	// Present / hide the late-2D UI layer (HackerContext::ActivateUILayer) as
	// one head-relative compositor quad.
	bool PresentUILayerOverlay(ID3D11Texture2D *texture);
	void HideUILayerOverlay();
	// The start-up fly-through before the main menu has been shown once and
	// before gameplay: Metro keeps Artyom's viewmodel out of sight there with
	// its own transform, which the VR hand placement would otherwise undo.
	bool IsStartupIntro();
	// The next frame's post-composite 2D goes to the UI-layer quad: a
	// non-gameplay 3D screen with a working OpenVR overlay (decided in
	// UpdateCinemaFrame; see HackerContext::ActivateUILayer).
	bool IsUILayerScreen();

	// Present-step trace in vr_compatibility_log.txt (always on, unbuffered).
	// Active for the first frames of a session and for a few frames after
	// ArmTrace, so a hang can be pinned to the exact step it happened in.
	void TraceStep(const char *step);
	void TraceFrameEnd();
	void ArmTrace(const char *reason, int frames);
	// Frames the OpenVR compositor has accepted so far this session.
	unsigned SuccessfulCompositorFrames();

	// Releases the private submission texture. Called when the swap
	// chain is going away/resizing so we don't hold a stale texture of
	// the wrong size.
	void ReleaseCompositorResources();

	// Metro2033ReduxVR milestone 2 (stereo): builds the per-eye
	// projection matrices and camera offsets in G, derived from the
	// HMD's real frustums but expressed in the game's own projection
	// convention and reusing the game's own near/far planes (recovered
	// from the matrix passed in). Call with the game's unmodified m_P;
	// no-op after the first successful call, and no-op without a
	// headset. Sets G->vrStereoParamsValid on success.
	void EnsureStereoParams(const float gameProjection4x4[16]);

	// Advances to the next eye. Called once per frame from Present, so
	// the frame rendered after it belongs to the other eye.
	void AdvanceStereoEye();

	// Metro2033ReduxVR motion-control feasibility work: reports the
	// camera's world position, recovered from the game's own view matrix
	// in PatchMappedVRCameraData. This is the anchor for locating the
	// game's player/camera state in memory - normally the hard part of a
	// memory search is knowing what value to look for, and we already
	// have it to full precision. Cheap; just stores the value.
	void NotifyCameraPosition(const float worldPos[3]);

	// The game's own view angles for this frame, in radians, derived from
	// its unmodified view rotation. A better search target than position:
	// the position scan found only stack temporaries, meaning the camera
	// position is recomputed per frame rather than stored - whereas view
	// angles are genuine persistent player state. They are also the value
	// motion controls would need to WRITE, so finding them answers the
	// feasibility question directly.
	void NotifyViewAngles(float yawRadians, float pitchRadians);

	// The camera's forward direction in world space - the current search
	// target, since the engine appears to store orientation as a vector
	// or matrix rather than as Euler angles.
	void NotifyViewForward(const float forward[3]);

	// The game's full 3x3 view rotation. A far more selective search
	// target than a vector: nine floats matching in sequence is
	// effectively free of false positives, and engines commonly store
	// camera orientation as a transform. Matched contiguously (a bare
	// 3x3) and with a stride of four (rows of a 3x4/4x4), against both
	// the matrix and its transpose, since we don't know whether the
	// engine keeps the world-to-view form or its inverse.
	void NotifyViewRotation(const float R[9]);

	// Metro2033ReduxVR motion-aiming feasibility: injects synthetic look
	// input and measures whether the game's own view angle responds.
	//
	// If the engine accepts injected input, motion aiming becomes possible
	// WITHOUT finding any memory address: compute the angle between where
	// the game is aiming and where the controller points, then inject
	// enough look input to close the gap. The game aims itself, using its
	// own code. If it ignores injected input, that route is closed and we
	// are back to memory work.
	//
	// Self-verifying: it compares the game's yaw before and after, so the
	// log states the answer outright rather than relying on eyeballing it.
	// Runs once, briefly, then stops.
	void RunInputInjectionTest();

	// Motion aiming: steers the game's aim toward wherever the right
	// controller points, by injecting look input each frame rather than
	// writing any game memory. The game aims itself, with its own code.
	//
	// A proportional closed loop - it measures the angle between the
	// game's current aim and the controller's direction and pushes to
	// close it - so it converges even though our per-unit calibration is
	// only approximate. Self-anchoring: the first update records the
	// current game aim and controller direction as the zero point, so no
	// assumption is needed about how the tracking frame relates to the
	// game's world.
	void UpdateMotionAiming();

	// Reads the VR controllers and injects movement, turning and action
	// input, so the game can be played without reaching for a gamepad.
	// Turning deliberately bypasses the aim loop's book-keeping - it is the
	// player rotating their body, so it should move the view and must not be
	// cancelled out of it.
	void UpdateControllerInput();

	// Called when the renderer sees the ammunition-magazine HUD icon's unique
	// shader pair. That marker is gameplay-only (unlike generic flat/world UI,
	// both of which Metro also emits in menus), so it safely enables the
	// culling-camera and locomotion compensation for the rest of the session.
	void NotifyGameplayHUDConfirmed();
	bool IsGameplayModeActive();
	bool IsWeaponInventoryHeld();
	bool IsInventoryMenuHeld();
	bool IsPauseMenuExpected();

	// The exact loading-panel shader is a stronger reload boundary than an
	// optional gameplay HUD element.  UpdateCullingFollow uses its appearance
	// and disappearance to clear a pause latch left behind by Reload/Load.
	void NotifyLoadingScreenDraw(unsigned frame);

	// Predict journal presentation on synthetic Back-button edges; physical
	// book presence expires stale requests after native closure/level changes.
	void NotifyJournalButton(unsigned frame);
	unsigned LastJournalButtonFrame();
	bool IsJournalPresented();
	// Physical book presence, independent of optional/delayed objective text.
	void NotifyJournalBookDraw(unsigned frame);

	// Diagnostic-only publication of the synthetic lighter action. The render
	// path uses its sequence/frame to capture settled lighter-on/off frames.
	void NotifyLighterButton(unsigned frame);
	unsigned LastLighterButtonFrame();
	unsigned LighterButtonSequence();
	// The renderer publishes the exact lighter-body draw every frame. This
	// keeps its temporary placement controls scoped to the lighter instead of
	// stealing the normal weapon-calibration chord when it is put away.
	// 0=gameplay regular, 1=gameplay journal, 2=prologue regular,
	// 3=prologue journal. Keeping the presentation and skeleton identities in
	// one value prevents prologue calibration from changing the main game.
	void MarkLighterBodyActive(unsigned lighterVariant = 0);
	// Temporary native gas-mask visual census: 0=inactive, 1=pre-equip
	// baseline while carried, 2=equip animation differential.
	unsigned GasMaskVisualProbePhase();
	// Read-only diagnostic state maintained by the working gesture path. Used
	// to compare stable render submissions with the mask off versus worn.
	bool GasMaskAssumedWornForDiagnostics();
	// Read Metro's authoritative active equipment-slot byte once per frame for
	// the temporary charger render differential. Returns false when the native
	// player/equipment state is unavailable rather than guessing.
	bool GetChargerOutForDiagnostics(bool *out);

	// Per-draw signal for the diegetic startup menu. Generic world UI is not
	// gameplay-exclusive, but its disappearance after the player presses
	// Confirm provides a reliable loading transition; the resulting gameplay
	// latch is one-way.
	void NotifyWorldPlacedUI();
	// Metro's authoritative game-manager menu mode. Unlike world-placed UI,
	// mode 1 is main-menu-specific; gameplay and the in-game menu are distinct.
	bool IsNativeMainMenuActive();

	// How many times the player has clicked the right stick to step the
	// viewmodel scale. The renderer owns the table of values; this is just the
	// index into it, so the two never have to agree on units.
	int GetWeaponScaleStep();

	// Whether the trigger is held. Lets the renderer tell which draws only
	// happen while shooting - which is what the muzzle flash is.
	bool IsFiring();

	// Whether aim-down-sights is held. Lets the renderer correlate the
	// viewmodel's transform with the engine's ADS animation.
	bool IsAiming();

	// Magnification requested by a positively identified scope configuration.
	// Unknown, iron-sight and reflex configurations always return 1x, so scope
	// detection never has to press Metro's aim control as a probe.
	float GetRequestedScopeMagnification();

	// Live weapon-position nudge in view-space metres, driven from the left
	// stick and added to kWeaponOffsetView. Logged whenever it changes, so
	// whatever position the player settles on can be read out and frozen.
	void GetWeaponOffsetAdjust(float out[3]);

	// Weapon rest-orientation offset in radians, {pitch, yaw}, for zeroing the
	// iron sights against where rounds actually land. Applied on the model side
	// of the controller rotation, so it is an offset in the weapon's own rest
	// frame and is independent of scale and position.
	void GetWeaponRotAdjust(float out[2]);

	// Persistent offsets for the weapon currently being rendered. These are
	// the values shown and edited by the VR menu; each complete weapon mesh set
	// owns a separate profile in vr_weapon_calibrations.txt.
	bool GetCurrentWeaponMenuAdjust(float position[3], float rotationDegrees[3]);
	bool AdjustCurrentWeaponMenuPosition(int axis, float deltaMetres);
	bool AdjustCurrentWeaponMenuRotation(int axis, float deltaDegrees);
	bool SaveCurrentWeaponMenuAdjust();
	// The normal left-hand mesh uses one controller-relative forced pose across
	// all weapons, so its menu editor writes the established global hand file.
	bool GetCurrentLeftHandMenuAdjust(float position[3], float rotationDegrees[3]);
	bool AdjustCurrentLeftHandMenuPosition(int axis, float deltaMetres);
	bool AdjustCurrentLeftHandMenuRotation(int axis, float deltaDegrees);
	bool SaveCurrentLeftHandMenuAdjust();

	// Feed each mesh belonging to the currently rendered held-weapon cluster
	// into the persistent calibration profiler. A saved profile is matched as
	// a set, allowing several weapons to be calibrated in one session.
	void ObserveWeaponCalibrationMesh(unsigned indexCount, int baseWeaponId = -1);
	// Returns the persistent key registered for a stable weapon-body draw, or
	// -1 when this body has not yet been registered by calibration.
	int GetRegisteredWeaponKey(unsigned indexCount);
	bool IsWeaponBodyRecentlySeen(unsigned maxAge = 2);
	void NoteViewmodelBodyDraw(unsigned indexCount);

	// The heading the player's body faces, in radians - the direction the
	// rendered view is anchored to. It moves only when the player
	// deliberately turns, never because the aim injection moved the camera.
	//
	// This is the whole trick. The engine welds aim to the camera and we
	// cannot unweld it, but we own what gets rendered: PatchMappedVRCameraData
	// rotates each frame's view so it faces this heading instead of wherever
	// the aim injection has pushed the camera, then composes the head
	// rotation on top. The headset shows where the player's HEAD is looking
	// while the game aims where the CONTROLLER points.
	//
	// Note the caller computes the correction itself, from the same frame's
	// camera rotation it is about to patch. Handing over a precomputed
	// correction instead meant deriving it from the previous frame's angles
	// and applying it to the next frame's matrix - a frame of drift, and the
	// residual view movement that survived several rounds of tuning.
	void GetBodyHeading(float *outYaw, float *outPitch, bool *valid);
	// Stable body heading captured from the camera matrix that was actually
	// patched for rendering. Projectile-origin translation consumes this so it
	// uses the exact same yaw as the rendered 6DOF camera.
	void SetRenderedBodyHeading(float yaw);
	bool GetRenderedBodyHeading(float *outYaw);

	// Head translation from the seated/reference pose, in game world axes.
	// This is the first-stage 6DOF camera delta; it is intentionally separate
	// from weapon translation and shot-origin logic.
	bool GetHeadTranslationDelta(float out[3]);
	// Same displacement expressed in the live HMD-local frame used by the
	// controller-relative weapon position.
	bool GetHeadTranslationDeltaInHeadFrame(float out[3]);

	// Resets every anchor to "here, now" - controller centre, body heading,
	// commanded total and the head reference. Bound to holding the right
	// controller's menu button, and run automatically when the game's camera
	// jumps (level load, death, cutscene) so a teleported camera is not
	// cancelled into a wildly wrong view.
	void RecenterAiming();
	// Monotonic generation advanced whenever recentering clears controller
	// anchors. Render-side hand placement uses it to discard its separately
	// cached authored basis on the same recalibration event.
	unsigned GetControllerRecenterGeneration();

	// Captures both eyes to disk on the next frame. Bound to holding the
	// right controller's menu button for about two seconds, so the moment is
	// chosen while standing in front of whatever needs measuring - a frame
	// count photographed the main menu instead.
	void RequestEyeDump();

	// Slowly steers the engine's camera to follow the head, so its frustum
	// culling covers what the player is actually looking at. Only possible now
	// that shot direction is independent of the camera (Notes/14).
	void UpdateCullingFollow();
	// Widen Metro's engine-side world projection before it performs CPU
	// frustum/portal visibility for the next frame. The rendered projection is
	// still replaced with the real HMD projection in HackerContext.
	void ForceWideCullingFov();
	// The engine's own camera position in world space, read straight from the
	// camera object found in Notes/14. Used to tell player-attached geometry
	// from world geometry: only the latter moves in view space when this does.
	bool GetCameraWorldPos(float out[3]);
	bool GetCameraForward(float out[3]);

	// The controller's rotation relative to the headset, in game view space,
	// measured from a rest pose captured once tracking settles. The weapon's
	// instance transform is local-to-VIEW, so this drops almost straight in.
	bool GetWeaponRotationDelta(float out[9]);

	// How far the controller has moved from its rest position, in game view
	// space - the translation half of 6DOF, and the same near-direct mapping
	// the rotation gets, since the viewmodel transform is local-to-view.
	bool GetWeaponTranslationDelta(float out[3]);
	bool GetLeftHandRotationDelta(float out[9]);
	bool GetRightHandRotationDelta(float out[9]);
	bool GetLeftHandTranslationDelta(float out[3]);
	bool GetLeftHandControllerRelativeToHead(float out[3]);
	bool GetLeftHandControllerRotationRelativeToHead(float out[9]);
	void GetLeftHandOffsetAdjust(float out[3]);
	void GetLeftHandRotationAdjust(float out[3]);
	void GetRightHandOffsetAdjust(float out[3]);
	void GetRightHandRotationAdjust(float out[3]);
	// The opening sequence uses a different skeleton and therefore owns a
	// separate hand calibration. The render path marks it as active so the
	// existing hand-calibration chord edits the correct file.
	void MarkPrologueLeftHandActive();
	void GetPrologueLeftHandOffsetAdjust(float out[3]);
	void GetPrologueLeftHandRotationAdjust(float out[3]);
	void GetWatchOffsetAdjust(float out[3]);
	void GetWatchAssemblyCalibration(float position[3], float rotation[3]);
	void GetPrologueWatchAssemblyCalibration(float position[3], float rotation[3]);
	void GetWatchTimeCalibration(float position[3], float rotation[3], float *scale);
	void GetLighterCalibration(float position[3], float rotation[3],
		unsigned lighterVariant = 0);
	// Charger placement owns independent assembly and right-hand profiles. The
	// exact charger body draw marks this state, so its calibration can preempt
	// the legacy LT+left-grip weapon chord without ever modifying a gun profile.
	void MarkChargerBodyActive();
	bool IsChargerVisualActive();
	void GetChargerCalibration(float position[3], float rotation[3]);
	void GetChargerRightHandCalibration(float position[3], float rotation[3]);
	void GetLighterFlameCalibration(float groupPosition[3],
		float secondaryPosition[3], float *scale,
		unsigned lighterVariant = 0);

	// The same measurement with NO rest anchor removed: where the controller
	// actually sits relative to the head, in game view axes and game units.
	// The weapon transform runs on deltas by design, so this exists only for
	// the task-27 calibration measurement - "is the gun where the hand is?"
	// is a question about absolute position and a delta cannot answer it.
	bool GetControllerRelativeToHead(float out[3]);
	// Absolute controller orientation in the same head-relative game-view
	// space as GetControllerRelativeToHead. Unlike GetWeaponRotationDelta this
	// retains the physical grip orientation instead of removing a rest pose.
	bool GetControllerRotationRelativeToHead(float out[9]);

	// Journal objective calibration. The render path announces the confirmed
	// objective draw; the controller-input path then owns both sticks only while
	// calibration is enabled and that draw remains live.
	void NotifyJournalObjectiveDraw(unsigned frameNo);
	void GetJournalCalibration(float position[3], float rotation[3], float *scale);

	// The rest pose the translation delta is measured from, for the same
	// measurement: gun position = rest + delta, so the rest term is half the
	// answer to where the gun ends up.
	bool GetWeaponRestAnchor(float out[3]);

	// Where the weapon is drawn in view space. Published by the renderer so the
	// shot can be traced from the gun rather than from the eye - the camera
	// sits well above the weapon, and tracing from it makes rounds land high
	// by an amount that varies with range.
	void SetWeaponViewPosition(const float v[3]);
	bool GetWeaponViewPosition(float out[3]);

	// How much yaw that has injected so far, cancelled out of the rendered
	// view so the injection cannot affect what the player sees.
	float GetCullingCancellation();
	float GetCullingCancellationPitch();
	// Candidate: credit associated with the replayed native camera generation.
	bool GetNativeCameraRenderCredit(const float *view3x4, float *yaw, float *pitch);

	// Pitch a scripted camera is contributing to the rendered view. Zero during
	// ordinary play; non-zero while a cutscene is aiming the camera, then
	// fading out as the follow loop re-converges on the head.
	// Log-only: how far the head has moved since the pose the view uses.
	void LogPoseStaleness();

	float GetScriptedPitchOffset();
	bool ScriptedCameraActive();
	// Exact native effector ownership may route a complete viewmodel. The
	// broader player-performance state is restricted to known hand/arm meshes.
	bool NativeScriptedViewmodelActive();
	bool ScriptedHandPerformanceActive();

	// Writes the controller's aim direction straight into the game's own
	// orientation vector, once the scanner has located and verified one.
	// Unlike input injection this does NOT move the camera - if the game
	// reads this for its shot direction, aim decouples from view, which is
	// the whole point.
	void ApplyAimOverride();

	// Self-verifying: writes a deliberately offset direction and measures
	// whether the game's own view follows, so the log states outright
	// whether this address has any effect. Needs no judgement from the
	// player and no working on-screen indicator.
	// Arms a hardware breakpoint on the address the engine is about to write
	// its view matrix to, so the trap lands inside the engine's OWN camera
	// code with every register intact. Value scans could only ever find
	// recomputed copies; this finds the code that produces them, and from
	// there the camera object itself. One-shot, read-only, self-disarming.
	// Writes the game's DECRYPTED code image to metro_dumped.bin, once, after
	// the probe fires. metro.exe is Steam-DRM encrypted on disk, so the only
	// readable copy of the engine's code is the one in this process. Flat
	// image at virtual addresses, so file offset == RVA.
	// Stage 2 of the reverse-engineering probe: breaks on a write to the
	// engine's OWN CPU-side copy of the view matrix, whose static pointer
	// stage 1's disassembly revealed. The code that trips it computes the
	// view matrix, and therefore reads the camera object.
	void ArmMatrixSourceProbe();
	void ReArmMatrixSourceProbe();
	void ArmCameraMatrixProbe();
	void ArmCommandStreamProbe();

	// Hunts for memory UPSTREAM of the camera - state the engine reads back,
	// rather than another copy that merely tracks it. Uses the camera object
	// as an oracle: writes a rotated direction into a candidate and checks
	// whether the engine's own camera follows, which tests causation instead
	// of correlation. Bounded at every stage and always reports a verdict.
	void RunPlayerStateHunt();

	// Finds the shot code by differencing which pages execute while firing
	// against which execute otherwise. Targets behaviour rather than storage,
	// so it is unaffected by the aim state being physics-owned or recomputed
	// - the reason every value scan came back empty.
	void RunFireCodeCoverage();

	// Stops at the engine's fire call and dumps the weapon object, to confirm
	// whether it carries the shot origin and direction as plain vectors. If it
	// does, aim can be redirected without touching the camera.
	void ArmFireProbe();

	void DumpGameModuleIfRequested();

	void ArmCameraWriteProbe(void *mappedData);

	void RunAimWriteTest();

	// Starts the background scan that hunts for the camera position in
	// the process's own memory. Safe to call every frame; only the first
	// call does anything. Runs off the render thread so it can't stall
	// the game, and only ever READS memory.
	void EnsureMemoryScannerStarted();

	// Attempts vr::VR_Init once. Safe to call every frame - after the
	// first attempt (success or failure) it's a cheap no-op check.
	// Called from HackerSwapChain::Present.
	void EnsureInitialised();

	// Pace the next OpenVR frame immediately after the companion-window
	// Present. OpenVR documents this as the correct point for WaitGetPoses;
	// calling it before Present can deadlock compositor implementations.
	void WaitForCompositorFrame();

	// Polls the HMD pose, captures a reference orientation on first
	// valid pose, and updates G->vrHeadRotation3x3 (the DELTA rotation
	// since that reference, in game-space convention) / G->vrHeadRotationValid.
	// No-op if OpenVR isn't initialised or the HMD pose isn't currently
	// valid (e.g. headset asleep/not worn).
	void UpdateVRPose();

	// Given a row-major 3x3 rotation R and 3-component translation T
	// describing a rigid transform [R|T] (world-to-view, matching the
	// game's m_V), computes the inverse transform's rotation (R^T) and
	// translation (-R^T * T). Used to keep m_iV consistent with a
	// patched m_V - see HackerContext::PatchMappedVRCameraData.
	void ComputeRigidInverse(const float R[9], const float T[3], float outR[9], float outT[3]);

	// Row-major 3x3 helpers used to compose the game's original rotation
	// with our tracked delta.
	void Transpose3x3(const float R[9], float outR[9]);
	void Multiply3x3(const float A[9], const float B[9], float outC[9]);

	// Composes two affine row-major 3x4 transforms (each implicitly a 4x4
	// with last row [0,0,0,1]): outC = A * B. Used for
	// cb_main_matrices0's m_WV = (new) view * world - see
	// HackerContext::PatchMappedVRObjectData.
	void Multiply3x4Affine(const float A[12], const float B[12], float outC[12]);

	// Multiplies a general 4x4 (P, e.g. a projection matrix - not
	// affine) by an affine row-major 3x4 (A, implicitly a 4x4 with last
	// row [0,0,0,1]): outC = P * A. Used for cb_main_matrices0's
	// m_WVP = projection * m_WV.
	void Multiply4x4Affine3x4(const float P[16], const float A[12], float outC[16]);

	// General row-major 4x4 product, outC = A * B. Needed for single-pass
	// stereo, where neither operand is affine: the reprojection that carries a
	// left-eye clip position to the right eye is
	//
	//     M = (P1 * V1) * (P0 * V0)^-1
	//
	// and both halves are full projection-view products.
	void Multiply4x4(const float A[16], const float B[16], float outC[16]);

	// General row-major 4x4 inverse by cofactors. Returns false if the matrix
	// is singular, in which case outM is untouched - the caller must not fall
	// back to garbage, since this feeds every vertex in the frame. A rigid
	// inverse would not do here: the operand includes a projection.
	bool Invert4x4(const float m[16], float outM[16]);

	// Promotes a row-major affine 3x4 to a full 4x4 with last row [0,0,0,1].
	void Promote3x4To4x4(const float A[12], float out[16]);

	// Metro2033ReduxVR: tracks which ID3D11InputLayout objects use the
	// particle/instanced-sprite system's per-instance "inst0..inst7"
	// semantic convention at IA slot 1 (billboard position/orientation
	// data, e.g. for fences and similar tiled decorative geometry - see
	// Notes/10) - NOT a local-to-view matrix, and corrupted if
	// HackerContext::PatchInstanceXformAtOffset blindly treats it as
	// one. Deliberately a BLOCKLIST rather than an allowlist: an
	// earlier allowlist approach (only patch layouts matching the
	// weapon's confirmed "xform0..xform3" naming) regressed the weapon
	// AND wrongly excluded other legitimate matrix-consuming layouts
	// this project has never sampled/named explicitly - every distinct
	// per-object shader family in this engine plausibly has its own
	// semantic naming, and enumerating all of them isn't tractable.
	// Blocking only the one specific, disassembly-confirmed non-matrix
	// case (Notes/10) restores the original inclusive-by-default
	// behavior everywhere else. Opaque void* here (rather than
	// ID3D11InputLayout*) to avoid a d3d11.h dependency in this header;
	// HackerDevice::CreateInputLayout does the actual semantic-name
	// inspection at layout-creation time.
	void RegisterKnownNonMatrixInputLayout(void *inputLayout);
	bool IsKnownNonMatrixInputLayout(void *inputLayout);

	// Metro2033ReduxVR TEMPORARY diagnostic (Notes/10 Update 5): tracks
	// layouts using the weapon's confirmed "xform0..xform3" naming
	// specifically (separate from the blocklist above), so
	// PatchInstanceXformAtOffset can write an INERT (unchanged) value
	// for the weapon's own offset while still performing the identical
	// Map(WRITE_NO_OVERWRITE)/Unmap cycle - isolating "does merely
	// touching this buffer for the weapon's data cause the prop
	// regression" from "does writing real corrected values to it".
	// Layouts that declare exactly FOUR elements on input slot 1 - a 4x4
	// per-instance matrix. This is what identifies a draw that actually
	// consumes the instance transform, as opposed to the many draws that
	// merely have slot 1 bound and ignore it. Element count rather than
	// semantic naming, per Notes/10: naming varies between shader families
	// and matched the wrong draws.
	void RegisterInstanceMatrixInputLayout(void *inputLayout);
	bool IsInstanceMatrixInputLayout(void *inputLayout);

	// Layouts that declare per-vertex bone weights or indices - i.e. skinned
	// meshes. Registered from CreateInputLayout, where the element semantics
	// are visible; at draw time only the layout pointer survives.
	//
	// Every NPC doubles in stereo while rigid world geometry is correct, and
	// the distortion is non-rigid, which is what a wrong transform does to a
	// skinned mesh specifically. Being able to say "this draw is skinned"
	// turns that from an observation into a test.
	void RegisterSkinnedInputLayout(void *inputLayout);
	bool IsSkinnedInputLayout(void *inputLayout);

	void RegisterKnownWeaponXformInputLayout(void *inputLayout);
	bool IsKnownWeaponXformInputLayout(void *inputLayout);

	bool NativeStateFollowSelected();

}
