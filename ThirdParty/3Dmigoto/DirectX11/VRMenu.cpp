#include "stdafx.h"
#include "VRMenu.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "log.h"
#include "VRPose.h"

namespace VRMenu {

namespace {
	Settings sSettings;
	bool sInitialized = false;
	bool sOpen = false;
	bool sChordLatched = false;
	ULONGLONG sChordHeldSince = 0;
	bool sLeftGripLatched = false;
	bool sRightGripLatched = false;
	int sCurrentTab = 0;
	int sSelectedRow = 0;
	bool sLeftYLatched = false;
	bool sRightXLatched = false;
	ULONGLONG sRightXRepeatAt = 0;
	int sRightXDirection = 0;
	bool sConfirmLatched = false;
	bool sCancelLatched = false;
	unsigned sCurrentResolutionWidth = 0;
	unsigned sCurrentResolutionHeight = 0;
	unsigned sRequestedOutputResolutionWidth = 0;
	unsigned sRequestedOutputResolutionHeight = 0;
	unsigned sResolutionBaseWidth = 0;
	unsigned sResolutionBaseHeight = 0;
	bool sSceneResolutionDetected = false;
	bool sSceneResolutionChangedAtRuntime = false;
	unsigned sLastScaledWidth = 0;
	unsigned sLastScaledHeight = 0;
	// Captured exactly once when the settings file is loaded. The renderer uses
	// this value for the lifetime of the process; sSettings.resolutionScale is
	// the saved next-launch value and may continue changing in the menu.
	float sAppliedResolutionScale = 1.0f;
	// Metro's broad visibility fixes patch executable code and cannot safely be
	// removed while render/worker threads may be executing it. Capture the saved
	// choice once at startup, just like resolution scale. Menu edits select the
	// mode for the next process and never create a mixed visibility state.
	bool sAppliedExpandedVisibility = true;
	// Metro's actual internal scene canvas.  The 2560x1421 value is a
	// viewport used by some passes; the render targets and final downsample
	// source are 3620x2009 at the native 1.00x setting.
	static const unsigned kNativeSceneWidth = 3620;
	static const unsigned kNativeSceneHeight = 2009;

	static const int kTabCount = 4;
	int RowCount()
	{
		// Picture, Controls, UI, Advanced. Display-only headings and the current
		// resolution readout are deliberately absent so selection and highlight
		// geometry always describe the same rows.
		static const int rows[kTabCount] = { 8, 6, 7, 16 };
		return rows[sCurrentTab];
	}

	bool IsHeldAdjustment()
	{
		// These rows accept horizontal right-stick adjustment with a short
		// repeat delay. Resolution presets are a five-position stepped control,
		// so include that button row alongside the continuous sliders; this lets
		// the player reach Low through Max without using the confirm button.
		// Leave radio buttons and checkboxes confirm-only for now.
		if (sCurrentTab == 0)
			return sSelectedRow == 0 || sSelectedRow == 1 ||
				sSelectedRow == 2 || sSelectedRow == 3 ||
				sSelectedRow == 4;
		if (sCurrentTab == 1)
			return sSelectedRow == 4;
		if (sCurrentTab == 2)
			return sSelectedRow >= 1 && sSelectedRow <= 3;
		if (sCurrentTab == 3)
			return (sSelectedRow >= 0 && sSelectedRow <= 5)
				|| (sSelectedRow >= 7 && sSelectedRow <= 12);
		return false;
	}

	void Adjust(float direction)
	{
		Settings &s = sSettings;
		switch (sCurrentTab) {
		case 0:
			if (sSelectedRow == 0) s.stereoSeparationMm = max(40.0f, min(90.0f, s.stereoSeparationMm + direction * 0.5f));
			else if (sSelectedRow == 1) s.resolutionScale = max(0.5f, min(2.0f, s.resolutionScale + direction * 0.05f));
			else if (sSelectedRow == 2) {
				s.resolutionPreset = max(0, min(4, s.resolutionPreset + (direction > 0 ? 1 : -1)));
				static const float presetScale[5] = { 0.50f, 0.75f, 1.00f, 1.50f, 2.00f };
				s.resolutionScale = presetScale[s.resolutionPreset];
			}
			else if (sSelectedRow == 3) s.worldScale = max(0.5f, min(2.0f, s.worldScale + direction * 0.05f));
			else if (sSelectedRow == 4) s.brightness = max(0.0f, min(1.0f, s.brightness + direction * 0.05f));
			else if (sSelectedRow == 5) s.brightnessCorrection = !s.brightnessCorrection;
			else if (sSelectedRow == 6) s.expandedVisibility = !s.expandedVisibility;
			else ResetCurrentCategory();
			break;
		case 1:
			if (sSelectedRow == 0) s.turning = s.turning == TurningMode::Smooth ? TurningMode::Snap : TurningMode::Smooth;
			else if (sSelectedRow == 1) { const int a[4] = {15,30,45,90}; int i=0; while (i<3 && a[i]!=s.snapAngle) ++i; s.snapAngle = a[(i + (direction > 0 ? 1 : 3)) % 4]; }
			else if (sSelectedRow == 2) s.swapAnalogSticks = !s.swapAnalogSticks;
			else if (sSelectedRow == 3) s.scopeForcedADS = !s.scopeForcedADS;
			else if (sSelectedRow == 4) s.scopeSensitivityMeters = max(0.01f,
				min(0.30f, s.scopeSensitivityMeters + direction * 0.01f));
			else if (sSelectedRow == 5) ResetCurrentCategory();
			break;
		case 2:
			if (sSelectedRow == 0) s.reticleEnabled = !s.reticleEnabled;
			else if (sSelectedRow == 1) s.hudPosition[0] += direction * 0.01f;
			else if (sSelectedRow == 2) s.hudPosition[1] += direction * 0.01f;
			else if (sSelectedRow == 3) s.hudSize = max(0.25f, min(2.0f, s.hudSize + direction * 0.05f));
			else if (sSelectedRow == 4) s.ammoCounterEnabled = !s.ammoCounterEnabled;
			else if (sSelectedRow == 5) s.perfHud = !s.perfHud;
			else if (sSelectedRow == 6) ResetCurrentCategory();
			// These are angular sight-zero offsets, not unrestricted HUD
			// translation. Keep calibration fine-grained and bounded for now.
			s.hudPosition[0] = max(-1.0f, min(1.0f, s.hudPosition[0]));
			s.hudPosition[1] = max(-1.0f, min(1.0f, s.hudPosition[1]));
			// The reticle is an infinite-distance ray and has no meaningful Z
			// coordinate. Keep the legacy saved component neutral and hidden.
			s.hudPosition[2] = 0.0f;
			break;
		case 3:
			if (sSelectedRow >= 0 && sSelectedRow <= 2)
				VRPose::AdjustCurrentWeaponMenuPosition(sSelectedRow, direction * 0.002f);
			else if (sSelectedRow >= 3 && sSelectedRow <= 5)
				VRPose::AdjustCurrentWeaponMenuRotation(sSelectedRow - 3, direction * 0.2f);
			else if (sSelectedRow == 6)
				VRPose::SaveCurrentWeaponMenuAdjust();
			else if (sSelectedRow >= 7 && sSelectedRow <= 9)
				VRPose::AdjustCurrentLeftHandMenuPosition(sSelectedRow - 7, direction * 0.002f);
			else if (sSelectedRow >= 10 && sSelectedRow <= 12)
				VRPose::AdjustCurrentLeftHandMenuRotation(sSelectedRow - 10, direction * 0.2f);
			else if (sSelectedRow == 13)
				VRPose::SaveCurrentLeftHandMenuAdjust();
			else if (sSelectedRow == 14) {
				float position[3] = {}, rotation[3] = {};
				VRPose::GetCurrentWeaponMenuAdjust(position, rotation);
				for (int axis = 0; axis < 3; ++axis) {
					VRPose::AdjustCurrentWeaponMenuPosition(axis, -position[axis]);
					VRPose::AdjustCurrentWeaponMenuRotation(axis, -rotation[axis]);
				}
				VRPose::GetCurrentLeftHandMenuAdjust(position, rotation);
				for (int axis = 0; axis < 3; ++axis) {
					VRPose::AdjustCurrentLeftHandMenuPosition(axis, -position[axis]);
					VRPose::AdjustCurrentLeftHandMenuRotation(axis, -rotation[axis]);
				}
			}
			else if (sSelectedRow == 15) ResetDefaults();
			break;
		}
		// Advanced offsets preview live and persist only through their explicit
		// Apply buttons. Other categories retain immediate persistence.
		if (sCurrentTab != 3) {
			Save();
		}
	}

	void SetDefaults(Settings &s)
	{
		memset(&s, 0, sizeof(s));
		s.renderMode = RenderMode::TrueStereo;
		s.aerPhase = AERPhase::AutoCapSync;
		s.stereoSeparationMm = 63.5f;
		s.worldScale = 1.0f;
		s.resolutionScale = 1.0f;
		s.resolutionPreset = 2; // Default compatibility setting (1.0x)
		s.brightness = 0.5f;
		s.brightnessCorrection = true;
		s.expandedVisibility = true;
		s.disableGamepadRightY = false;
		s.scopeForcedADS = true;
		s.scopeSensitivityMeters = 0.05f;
		s.turning = TurningMode::Smooth;
		s.snapAngle = 30;
		s.reticleEnabled = true;
		s.ammoCounterEnabled = true;
		s.hudSize = 1.0f;
	}

	const char *Path()
	{
		return "vr_menu_settings.txt";
	}

	void ReadFloat(FILE *f, const char *key, float *out)
	{
		char line[256];
		char name[128];
		const long pos = ftell(f);
		if (pos < 0) return;
		fseek(f, 0, SEEK_SET);
		while (fgets(line, sizeof(line), f)) {
			float v = 0.0f;
			if (sscanf_s(line, "%127[^=]=%f", name, (unsigned)sizeof(name), &v) == 2
				&& _stricmp(name, key) == 0) { *out = v; break; }
		}
		fseek(f, pos, SEEK_SET);
	}

	void ReadInt(FILE *f, const char *key, int *out)
	{
		float v = 0.0f;
		ReadFloat(f, key, &v);
		*out = (int)v;
	}

	void ReadBool(FILE *f, const char *key, bool *out)
	{
		int v = *out ? 1 : 0;
		ReadInt(f, key, &v);
		*out = v != 0;
	}

	void ReadVec3(FILE *f, const char *key, float out[3])
	{
		char line[256];
		char name[128];
		const long pos = ftell(f);
		if (pos < 0) return;
		fseek(f, 0, SEEK_SET);
		while (fgets(line, sizeof(line), f)) {
			float x = 0.0f, y = 0.0f, z = 0.0f;
			if (sscanf_s(line, "%127[^=]=%f,%f,%f", name, (unsigned)sizeof(name), &x, &y, &z) == 4
				&& _stricmp(name, key) == 0) {
				out[0] = x; out[1] = y; out[2] = z;
				break;
			}
		}
		fseek(f, pos, SEEK_SET);
	}
}

void Save()
{
	if (!sInitialized) return;
	FILE *f = NULL;
	if (fopen_s(&f, Path(), "w") != 0 || !f) return;
	const Settings &s = sSettings;
	fprintf(f, "render_mode=%d\naer_phase=%d\nstereo_separation_mm=%.4f\nworld_scale=%.4f\n",
		(int)s.renderMode, (int)s.aerPhase, s.stereoSeparationMm, s.worldScale);
	fprintf(f, "resolution_scale=%.4f\nresolution_preset=%d\nbrightness=%.4f\nbrightness_correction=%d\nexpanded_visibility=%d\n",
		s.resolutionScale, s.resolutionPreset, s.brightness,
		s.brightnessCorrection ? 1 : 0, s.expandedVisibility ? 1 : 0);
	fprintf(f, "gamepad_mode=%d\ndisable_gamepad_right_y=%d\nscope_forced_ads=%d\nscope_sensitivity_meters=%.4f\nleft_handed_mode=%d\nswap_analog_sticks=%d\n",
		s.gamepadMode ? 1 : 0, s.disableGamepadRightY ? 1 : 0,
		s.scopeForcedADS ? 1 : 0, s.scopeSensitivityMeters,
		s.leftHandedMode ? 1 : 0,
		s.swapAnalogSticks ? 1 : 0);
	fprintf(f, "turning=%d\nsnap_angle=%d\nreticle_enabled=%d\nammo_counter_enabled=%d\nhud_size=%.4f\n",
		(int)s.turning, s.snapAngle, s.reticleEnabled ? 1 : 0,
		s.ammoCounterEnabled ? 1 : 0, s.hudSize);
	fprintf(f, "weapon_position=%.5f,%.5f,%.5f\nweapon_rotation=%.5f,%.5f,%.5f\n",
		s.weaponPosition[0], s.weaponPosition[1], s.weaponPosition[2],
		s.weaponRotation[0], s.weaponRotation[1], s.weaponRotation[2]);
	fprintf(f, "hud_position=%.5f,%.5f,%.5f\n",
		s.hudPosition[0], s.hudPosition[1], s.hudPosition[2]);
	fprintf(f, "perf_hud=%d\n", s.perfHud ? 1 : 0);
	fclose(f);
}

bool ClampOversizedOutputResolution(unsigned *width, unsigned *height)
{
	if (!width || !height || *width == 0 || *height == 0)
		return false;
	static const unsigned kNativeOutputWidth = 2560;
	static const unsigned kNativeOutputHeight = 1440;
	if (*width <= kNativeOutputWidth && *height <= kNativeOutputHeight)
		return false;

	const unsigned requestedWidth = *width;
	const unsigned requestedHeight = *height;
	*width = kNativeOutputWidth;
	*height = kNativeOutputHeight;
	LogInfo("VR resolution: clamped oversized output %ux%u -> %ux%u "
		"(DSR/DLDSR guard)\n", requestedWidth, requestedHeight,
		*width, *height);
	return true;
}

bool ForceVRPresentationResolution(unsigned *width, unsigned *height)
{
	if (!width || !height)
		return false;
	static const unsigned kVRPresentationWidth = 2560;
	static const unsigned kVRPresentationHeight = 1440;
	if (*width == kVRPresentationWidth &&
		*height == kVRPresentationHeight)
		return false;

	const unsigned requestedWidth = *width;
	const unsigned requestedHeight = *height;
	*width = kVRPresentationWidth;
	*height = kVRPresentationHeight;
	LogInfo("VR resolution: presentation backbuffer %ux%u -> %ux%u "
		"(physical display target unchanged)\n", requestedWidth,
		requestedHeight, *width, *height);
	return true;
}

void SetRequestedOutputResolution(unsigned width, unsigned height)
{
	if (width < 640 || height < 360)
		return;
	sRequestedOutputResolutionWidth = width;
	sRequestedOutputResolutionHeight = height;
}

unsigned RequestedOutputResolutionWidth()
{
	return sRequestedOutputResolutionWidth;
}

unsigned RequestedOutputResolutionHeight()
{
	return sRequestedOutputResolutionHeight;
}

void Initialize()
{
	if (sInitialized) return;
	SetDefaults(sSettings);
	FILE *f = NULL;
	if (fopen_s(&f, Path(), "r") == 0 && f) {
		ReadInt(f, "render_mode", (int *)&sSettings.renderMode);
		ReadInt(f, "aer_phase", (int *)&sSettings.aerPhase);
		ReadFloat(f, "stereo_separation_mm", &sSettings.stereoSeparationMm);
		ReadFloat(f, "world_scale", &sSettings.worldScale);
		ReadFloat(f, "resolution_scale", &sSettings.resolutionScale);
		ReadInt(f, "resolution_preset", &sSettings.resolutionPreset);
		ReadFloat(f, "brightness", &sSettings.brightness);
		ReadBool(f, "brightness_correction", &sSettings.brightnessCorrection);
		ReadBool(f, "expanded_visibility", &sSettings.expandedVisibility);
		ReadBool(f, "gamepad_mode", &sSettings.gamepadMode);
		ReadBool(f, "disable_gamepad_right_y", &sSettings.disableGamepadRightY);
		ReadBool(f, "scope_forced_ads", &sSettings.scopeForcedADS);
		ReadFloat(f, "scope_sensitivity_meters", &sSettings.scopeSensitivityMeters);
		ReadBool(f, "left_handed_mode", &sSettings.leftHandedMode);
		ReadBool(f, "swap_analog_sticks", &sSettings.swapAnalogSticks);
		ReadInt(f, "turning", (int *)&sSettings.turning);
		ReadInt(f, "snap_angle", &sSettings.snapAngle);
		ReadBool(f, "reticle_enabled", &sSettings.reticleEnabled);
		ReadBool(f, "ammo_counter_enabled", &sSettings.ammoCounterEnabled);
		ReadFloat(f, "hud_size", &sSettings.hudSize);
		ReadVec3(f, "weapon_position", sSettings.weaponPosition);
		ReadVec3(f, "weapon_rotation", sSettings.weaponRotation);
		ReadVec3(f, "hud_position", sSettings.hudPosition);
		ReadBool(f, "perf_hud", &sSettings.perfHud);
		fclose(f);
	}
	sInitialized = true;
	// AER is not exposed because the current implementation is broken. Migrate
	// any older saved selection back to the only supported render mode.
	sSettings.renderMode = RenderMode::TrueStereo;
	// The primary package is now exclusively the 6DOF controller mod. Keep the
	// old implementation dormant for extraction into a separate gamepad build,
	// but never allow a persisted setting from an older build to enter that
	// hybrid path here.
	const bool retiredGamepadModeWasSaved = sSettings.gamepadMode;
	const bool retiredLeftHandedModeWasSaved = sSettings.leftHandedMode;
	sSettings.gamepadMode = false;
	sSettings.disableGamepadRightY = false;
	sSettings.leftHandedMode = false;
	sSettings.resolutionScale = max(0.5f, min(2.0f, sSettings.resolutionScale));
	sSettings.resolutionPreset = max(0, min(4, sSettings.resolutionPreset));
	sSettings.scopeSensitivityMeters = max(0.01f,
		min(0.30f, sSettings.scopeSensitivityMeters));
	sAppliedResolutionScale = sSettings.resolutionScale;
	LogInfo("VR resolution: startup scale latched=%.4f preset=%d\n",
		sAppliedResolutionScale, sSettings.resolutionPreset);
	sAppliedExpandedVisibility = sSettings.expandedVisibility;
	LogInfo("VR visibility: startup expanded mode latched=%d\n",
		sAppliedExpandedVisibility ? 1 : 0);
	if (retiredGamepadModeWasSaved || retiredLeftHandedModeWasSaved) {
		LogInfo("VRPose menu: retired saved optional mode from primary 6DOF build"
			" gamepad=%d leftHanded=%d\n",
			retiredGamepadModeWasSaved ? 1 : 0,
			retiredLeftHandedModeWasSaved ? 1 : 0);
		Save();
	}
	// Use Metro's native scene canvas for the target-resolution readout. The
	// submitted eye image is 2560x1440, but the game scene is 3620x2009.
	sResolutionBaseWidth = kNativeSceneWidth;
	sResolutionBaseHeight = kNativeSceneHeight;
}

const Settings &GetSettings() { Initialize(); return sSettings; }
Settings &MutableSettings() { Initialize(); return sSettings; }
float ResolutionScale() { Initialize(); return sAppliedResolutionScale; }
bool ExpandedVisibilityActive()
{
	Initialize();
	return sAppliedExpandedVisibility;
}
bool ExpandedVisibilityChangePending()
{
	Initialize();
	return sSettings.expandedVisibility != sAppliedExpandedVisibility;
}
float EffectiveSceneResolutionScale()
{
	Initialize();
	// Metro rebuilds its complete render-target graph when SSAA changes.  Do not
	// keep applying the old high-resolution graph assumptions during that live
	// transition: use a readable native fallback until the next process start,
	// where the newly selected SSAA canvas is detected cleanly from frame zero.
	return sSceneResolutionDetected && !sSceneResolutionChangedAtRuntime
		? sAppliedResolutionScale : 1.0f;
}
float CompositorResolutionScale()
{
	Initialize();
	// Keep the menu rasterized into at least the native per-eye submission.
	// At LOW/MED the scene may still be rendered at its selected scale, but the
	// overlay is no longer downsampled with it and remains readable in-headset.
	if (sOpen)
		return 1.0f;
	// Below 1x, retain the proven post-render downsample path. Above 1x,
	// submit the native-sized eye image and let Metro's r_supersample render
	// the extra scene pixels before its own resolve instead of upscaling an
	// already-rendered compositor texture.
	return min(1.0f, sAppliedResolutionScale);
}
bool ResolutionChangePending()
{
	Initialize();
	return fabsf(sSettings.resolutionScale - sAppliedResolutionScale) > 0.0001f;
}

void SetCurrentResolution(unsigned width, unsigned height)
{
	if (width < 640 || height < 360)
		return;
	sCurrentResolutionWidth = width;
	sCurrentResolutionHeight = height;
}

unsigned CurrentResolutionWidth() { return sCurrentResolutionWidth; }
unsigned CurrentResolutionHeight() { return sCurrentResolutionHeight; }

void SetDetectedSceneResolution(unsigned width, unsigned height)
{
	Initialize();
	if (width < 640 || height < 360)
		return;
	const bool changed = sSceneResolutionDetected &&
		(sResolutionBaseWidth != width || sResolutionBaseHeight != height);
	if (!sSceneResolutionDetected || sResolutionBaseWidth != width ||
		sResolutionBaseHeight != height) {
		LogInfo("VR resolution: detected Metro SSAA scene canvas %ux%u\n",
			width, height);
	}
	if (changed && !sSceneResolutionChangedAtRuntime) {
		sSceneResolutionChangedAtRuntime = true;
		LogInfo("VR resolution: Metro SSAA changed at runtime; using native "
			"fallback until restart\n");
	}
	sResolutionBaseWidth = width;
	sResolutionBaseHeight = height;
	sLastScaledWidth = sLastScaledHeight = 0;
	sSceneResolutionDetected = true;
}

bool HasDetectedSceneResolution()
{
	Initialize();
	return sSceneResolutionDetected;
}

unsigned DetectedSceneResolutionWidth()
{
	Initialize();
	return sSceneResolutionDetected ? sResolutionBaseWidth : 0;
}

unsigned DetectedSceneResolutionHeight()
{
	Initialize();
	return sSceneResolutionDetected ? sResolutionBaseHeight : 0;
}

float SceneCanvasScaleToReference()
{
	Initialize();
	if (!sSceneResolutionDetected || !sResolutionBaseWidth)
		return 1.0f;
	return (float)kNativeSceneWidth / (float)sResolutionBaseWidth;
}

bool IsBaseSceneResolution(unsigned width, unsigned height)
{
	Initialize();
	return sSceneResolutionDetected && width == sResolutionBaseWidth &&
		height == sResolutionBaseHeight;
}

bool IsSceneResolution(unsigned width, unsigned height)
{
	Initialize();
	if (!sSceneResolutionDetected)
		return false;
	if (width == sResolutionBaseWidth && height == sResolutionBaseHeight)
		return true;
	const float scale = EffectiveSceneResolutionScale();
	const unsigned scaledWidth = max(1u, (unsigned)floorf(
		sResolutionBaseWidth * scale + 0.5f));
	const unsigned scaledHeight = max(1u, (unsigned)floorf(
		sResolutionBaseHeight * scale + 0.5f));
	return width == scaledWidth && height == scaledHeight;
}

bool ScaleRenderResolution(unsigned baseWidth, unsigned baseHeight,
	unsigned *scaledWidth, unsigned *scaledHeight)
{
	Initialize();
	if (!scaledWidth || !scaledHeight || baseWidth < 640 || baseHeight < 360)
		return false;
	// ResizeBuffers may feed the dimensions returned by the previous creation
	// back to us. Preserve the original game request so scaling never compounds
	// across alt-tab, video playback, or a device recreation.
	if (sResolutionBaseWidth == 0 || sResolutionBaseHeight == 0
		|| (baseWidth != sResolutionBaseWidth && baseWidth != sLastScaledWidth)
		|| (baseHeight != sResolutionBaseHeight && baseHeight != sLastScaledHeight)) {
		sResolutionBaseWidth = baseWidth;
		sResolutionBaseHeight = baseHeight;
	}
	const float scale = CompositorResolutionScale();
	// Keep the exact scaled dimensions. Metro's native scene canvas has an odd
	// height (2009/1421), and changing it by one pixel breaks its resolve chain.
	unsigned width = max(640u, (unsigned)floorf(sResolutionBaseWidth * scale + 0.5f));
	unsigned height = max(360u, (unsigned)floorf(sResolutionBaseHeight * scale + 0.5f));
	*scaledWidth = width;
	*scaledHeight = height;
	sLastScaledWidth = width;
	sLastScaledHeight = height;
	return width != baseWidth || height != baseHeight;
}

bool CalculateResolutionForScale(float scale, unsigned *width, unsigned *height)
{
	if (!width || !height || sResolutionBaseWidth < 640 || sResolutionBaseHeight < 360)
		return false;
	scale = max(0.5f, min(2.0f, scale));
	unsigned w = max(640u, (unsigned)floorf(sResolutionBaseWidth * scale + 0.5f));
	unsigned h = max(360u, (unsigned)floorf(sResolutionBaseHeight * scale + 0.5f));
	*width = w;
	*height = h;
	return true;
}

void ResetDefaults()
{
	SetDefaults(sSettings);
	Save();
}

void ResetCurrentCategory()
{
	Initialize();
	Settings defaults;
	SetDefaults(defaults);
	switch (sCurrentTab) {
	case 0:
		sSettings.stereoSeparationMm = defaults.stereoSeparationMm;
		sSettings.resolutionScale = defaults.resolutionScale;
		sSettings.resolutionPreset = defaults.resolutionPreset;
		sSettings.worldScale = defaults.worldScale;
		sSettings.brightness = defaults.brightness;
		sSettings.brightnessCorrection = defaults.brightnessCorrection;
		sSettings.expandedVisibility = defaults.expandedVisibility;
		break;
	case 1:
		sSettings.gamepadMode = defaults.gamepadMode;
		sSettings.disableGamepadRightY = defaults.disableGamepadRightY;
		sSettings.scopeForcedADS = defaults.scopeForcedADS;
		sSettings.scopeSensitivityMeters = defaults.scopeSensitivityMeters;
		sSettings.leftHandedMode = defaults.leftHandedMode;
		sSettings.swapAnalogSticks = defaults.swapAnalogSticks;
		sSettings.turning = defaults.turning;
		sSettings.snapAngle = defaults.snapAngle;
		break;
	case 2:
		sSettings.reticleEnabled = defaults.reticleEnabled;
		sSettings.ammoCounterEnabled = defaults.ammoCounterEnabled;
		memcpy(sSettings.hudPosition, defaults.hudPosition,
			sizeof(sSettings.hudPosition));
		sSettings.hudSize = defaults.hudSize;
		sSettings.perfHud = defaults.perfHud;
		break;
	case 3:
		memcpy(sSettings.weaponPosition, defaults.weaponPosition,
			sizeof(sSettings.weaponPosition));
		memcpy(sSettings.weaponRotation, defaults.weaponRotation,
			sizeof(sSettings.weaponRotation));
		break;
	}
	Save();
}

void UpdateToggleInput(bool leftStickClick, bool rightStickClick,
	bool leftGrip, bool rightGrip)
{
	Initialize();
	const bool chord = leftStickClick && rightStickClick;
	if (chord) {
		const ULONGLONG now = GetTickCount64();
		if (!sChordHeldSince) {
			sChordHeldSince = now;
		}
		if (!sChordLatched && now - sChordHeldSince >= 1000) {
			sOpen = !sOpen;
			sChordLatched = true;
			LogInfo("VRPose menu: %s (L3+R3 held 1 second)\n",
				sOpen ? "OPEN" : "CLOSED");
		}
	} else {
		sChordHeldSince = 0;
		sChordLatched = false;
	}
	if (sOpen) {
	if (leftGrip && !sLeftGripLatched) {
			sCurrentTab = (sCurrentTab + kTabCount - 1) % kTabCount;
			sSelectedRow = 0;
			sRightXLatched = false;
		}
		if (rightGrip && !sRightGripLatched) {
			sCurrentTab = (sCurrentTab + 1) % kTabCount;
			sSelectedRow = 0;
			sRightXLatched = false;
		}
	}
	sLeftGripLatched = leftGrip;
	sRightGripLatched = rightGrip;
}

void UpdateTabNavigation(bool previous, bool next)
{
	Initialize();
	if (!sOpen) return;
	static bool previousLatched = false;
	static bool nextLatched = false;
	if (previous && !previousLatched) {
		sCurrentTab = (sCurrentTab + kTabCount - 1) % kTabCount;
		sSelectedRow = 0;
		sRightXLatched = false;
	}
	if (next && !nextLatched) {
		sCurrentTab = (sCurrentTab + 1) % kTabCount;
		sSelectedRow = 0;
		sRightXLatched = false;
	}
	previousLatched = previous;
	nextLatched = next;
}

bool IsOpen() { return sOpen; }
void SetOpen(bool open) { sOpen = open; }
int CurrentTab() { return sCurrentTab; }
int SelectedRow() { return sSelectedRow; }

void UpdateNavigation(float leftX, float leftY, float rightX,
	bool confirm, bool cancel)
{
	Initialize();
	if (!sOpen) return;
	const float dead = 0.55f;
	if (fabsf(leftY) < dead) sLeftYLatched = false;
	else if (!sLeftYLatched) {
		sSelectedRow += leftY > 0 ? -1 : 1;
		if (sSelectedRow < 0) sSelectedRow = RowCount() - 1;
		if (sSelectedRow >= RowCount()) sSelectedRow = 0;
		sLeftYLatched = true;
		LogInfo("VRPose menu: selected tab=%d row=%d\n", sCurrentTab, sSelectedRow);
	}
	if (fabsf(rightX) < dead) {
		sRightXLatched = false;
		sRightXRepeatAt = 0;
		sRightXDirection = 0;
	} else {
		const int direction = rightX > 0 ? 1 : -1;
		const ULONGLONG now = GetTickCount64();
		const bool directionChanged = direction != sRightXDirection;
		const bool firstPress = !sRightXLatched;
		const bool repeat = IsHeldAdjustment() && sRightXLatched && !directionChanged
			&& now >= sRightXRepeatAt;
		if ((firstPress && IsHeldAdjustment()) ||
			(directionChanged && IsHeldAdjustment()) || repeat) {
			Adjust((float)direction);
			sRightXLatched = true;
			sRightXDirection = direction;
			// One immediate step, then a short initial pause and smooth repeats.
			sRightXRepeatAt = now + (IsHeldAdjustment() ? (firstPress ? 250 : 55) : 0);
			LogInfo("VRPose menu: adjusted tab=%d row=%d\n", sCurrentTab, sSelectedRow);
		}
	}
	if (confirm && !sConfirmLatched) { Adjust(1.0f); sConfirmLatched = true; LogInfo("VRPose menu: activated tab=%d row=%d\n", sCurrentTab, sSelectedRow); }
	if (!confirm) sConfirmLatched = false;
	if (cancel && !sCancelLatched) { ResetDefaults(); sCancelLatched = true; }
	if (!cancel) sCancelLatched = false;
}

}
