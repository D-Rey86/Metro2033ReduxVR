#pragma once

// Persistent state and input ownership for the in-game VR menu.  Rendering
// deliberately lives in HackerContext so it can share the already-proven
// stereo overlay path; this module owns settings and controller semantics.

namespace VRMenu {

enum class RenderMode {
	TrueStereo = 0,
	AlternateEye = 1,
	Monoscopic = 2,
};

enum class AERPhase {
	LeftFirst = 0,
	RightFirst = 1,
	AutoCapSync = 2,
};

enum class TurningMode {
	Smooth = 0,
	Snap = 1,
};

struct Settings {
	RenderMode renderMode;
	AERPhase aerPhase;
	float stereoSeparationMm;
	float worldScale;
	float resolutionScale;
	int resolutionPreset;
	float brightness;
	bool brightnessCorrection;
	bool expandedVisibility;
	bool gamepadMode;
	bool disableGamepadRightY;
	bool scopeForcedADS;
	float scopeSensitivityMeters;
	bool leftHandedMode;
	bool swapAnalogSticks;
	TurningMode turning;
	int snapAngle;
	float weaponPosition[3];
	float weaponRotation[3];
	bool reticleEnabled;
	bool ammoCounterEnabled;
	float hudPosition[3];
	float hudSize;
};

void Initialize();
const Settings &GetSettings();
Settings &MutableSettings();
// ResolutionScale is the scale latched from disk during Initialize(). Menu
// edits update Settings::resolutionScale for the next launch, but never change
// this applied value in the running process.
float ResolutionScale();
// Expanded visibility installs several executable culling bypasses. The saved
// choice is therefore latched at startup and menu changes apply next launch.
bool ExpandedVisibilityActive();
bool ExpandedVisibilityChangePending();
// The scene scale is only safe to use after Metro's active SSAA canvas has
// been identified. Before that point this returns 1.0 so an unfamiliar SSAA
// mode falls back to a readable native image instead of applying mismatched
// viewport/UI compensation.
float EffectiveSceneResolutionScale();
float CompositorResolutionScale();
bool ResolutionChangePending();
void ResetDefaults();
void ResetCurrentCategory();
void Save();

// Metro may select an NVIDIA DSR/DLDSR fullscreen mode when its Video menu is
// opened even if the user changes nothing. Keep the VR presentation surface at
// the known physical output size; this does not touch internal scene targets.
bool ClampOversizedOutputResolution(unsigned *width, unsigned *height);

// Updated from the live swap-chain path so the Picture tab reports the
// resolution the game is actually presenting, rather than a hard-coded value.
void SetCurrentResolution(unsigned width, unsigned height);
unsigned CurrentResolutionWidth();
unsigned CurrentResolutionHeight();

// Metro changes its internal scene canvas with the SSAA option. The renderer
// reports the active base here at startup and whenever the Video menu rebuilds
// it, replacing the historical X2-only 3620x2009 assumption.
void SetDetectedSceneResolution(unsigned width, unsigned height);
bool HasDetectedSceneResolution();
unsigned DetectedSceneResolutionWidth();
unsigned DetectedSceneResolutionHeight();
// Ratio from the detected Metro canvas to the X2 canvas on which the VR UI
// placement was authored. This is 1.0 at X2 and about sqrt(2) at SSAA Off.
float SceneCanvasScaleToReference();
bool IsBaseSceneResolution(unsigned width, unsigned height);
bool IsSceneResolution(unsigned width, unsigned height);

// Converts Metro's requested swap-chain dimensions into the headset render
// dimensions selected in the Picture tab. Applied when the swap chain is
// created/recreated, so changing the slider takes effect on the next launch.
bool ScaleRenderResolution(unsigned baseWidth, unsigned baseHeight,
	unsigned *scaledWidth, unsigned *scaledHeight);
bool CalculateResolutionForScale(float scale, unsigned *width, unsigned *height);

// L3+R3 toggles the menu only after a one-second hold, preventing accidental
// openings from quick simultaneous stick clicks.
	void UpdateToggleInput(bool leftStickClick, bool rightStickClick,
		bool leftGrip, bool rightGrip);
	void UpdateTabNavigation(bool previous, bool next);
bool IsOpen();
void SetOpen(bool open);
int CurrentTab();
int SelectedRow();
void UpdateNavigation(float leftX, float leftY, float rightX,
	bool confirm, bool cancel);

}
