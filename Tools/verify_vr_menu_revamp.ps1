$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$menuHeader = [IO.File]::ReadAllText((Join-Path $root 'ThirdParty\3Dmigoto\DirectX11\VRMenu.h'))
$menuSource = [IO.File]::ReadAllText((Join-Path $root 'ThirdParty\3Dmigoto\DirectX11\VRMenu.cpp'))
$drawSource = [IO.File]::ReadAllText((Join-Path $root 'ThirdParty\3Dmigoto\DirectX11\HackerContext.cpp'))
$poseHeader = [IO.File]::ReadAllText((Join-Path $root 'ThirdParty\3Dmigoto\DirectX11\VRPose.h'))
$poseSource = [IO.File]::ReadAllText((Join-Path $root 'ThirdParty\3Dmigoto\DirectX11\VRPose.cpp'))
$visibilitySource = [IO.File]::ReadAllText((Join-Path $root 'ThirdParty\3Dmigoto\DirectX11\NativeVisibilityCoverage.inl'))

foreach ($required in @(
    'static const int kTabCount = 4;',
    'static const int rows[kTabCount] = { 8, 6, 6, 16 };',
    'sSettings.renderMode = RenderMode::TrueStereo;',
    'expanded_visibility=%d',
	'if (sCurrentTab != 3)',
	'ammo_counter_enabled=%d',
	's.hudPosition[2] = 0.0f;',
	'scope_forced_ads=%d',
	'scope_sensitivity_meters=%.4f',
    'if (sOpen)',
    'return 1.0f;'
)) {
    if (!$menuSource.Contains($required)) { throw "Missing VR menu behavior: $required" }
}
foreach ($removed in @('"RENDER", "PICTURE", "MOTION", "OFFSETS", "UI"', 'choice("AER"', 'checkAt("GAMEPAD MODE"', 'checkAt("SEPARATE HANDS"', 'checkAt("LEFT HANDED MODE"', 'textureQuality', 'lightingDistanceScale', 'shadowQuality', 'SyncNativeQualitySettings')) {
    if ($drawSource.Contains($removed)) { throw "Retired VR menu UI remains: $removed" }
}

foreach ($required in @(
	'const bool retiredGamepadModeWasSaved = sSettings.gamepadMode;',
	'const bool retiredLeftHandedModeWasSaved = sSettings.leftHandedMode;',
	'sSettings.gamepadMode = false;',
	'sSettings.disableGamepadRightY = false;',
	'sSettings.leftHandedMode = false;'
)) {
	if (!$menuSource.Contains($required)) {
		throw "Primary 6DOF build does not retire Gamepad Mode safely: $required"
	}
}

foreach ($required in @(
    '"PICTURE", "CONTROLS", "UI", "ADVANCED"',
	'METRO RENDER',
	'HEADSET OUTPUT',
	'1.5 RECOMMENDED FOR IMAGE QUALITY',
	'"LOW","MEDIUM","DEFAULT","QUALITY","MAX"',
	'DirectX::SpriteFont',
	'VRPose::PresentMenuOverlay(mVRMenuTexture)',
	'sliderAt("HORIZONTAL AIM"',
	'sliderAt("VERTICAL AIM"',
    'EXPANDED VISIBILITY',
    'FIXES EDGE POP IN  MAY LOWER FRAMERATE',
    'sliderAt("SCOPE SENSITIVITY METERS"',
    'RESET ALL VALUES',
    'std::vector<RowBounds> rows'
)) {
    if (!$drawSource.Contains($required)) { throw "Missing VR menu presentation: $required" }
}
if ($drawSource.Contains('sliderAt("POSITION Z"')) {
	throw 'Inactive reticle Position Z row remains exposed.'
}

foreach ($required in @(
	'APPLY WEAPON',
	'APPLY LEFT HAND',
	'GetCurrentWeaponMenuAdjust(weaponPosition,weaponRotation)',
	'GetCurrentLeftHandMenuAdjust(leftPosition,leftRotation)',
	'weaponReady'
)) {
	if (!$drawSource.Contains($required)) {
		throw "Missing Advanced offset presentation: $required"
	}
}

foreach ($required in @(
	'const UINT watchStartInstance = 0;',
	'Archives exposed this on the',
	'Every private single-instance',
	'const bool archivesSpartanWatchDraw =',
	'memcpy(finalWatch, sFinalWatch6108Instance, sizeof(finalWatch));',
	'every child has exactly the same rigid parent'
)) {
	if (!$drawSource.Contains($required)) {
        throw "Missing watch attachment guard: $required"
	}
}
foreach ($required in @(
	'VRPose::AdjustCurrentWeaponMenuPosition',
	'VRPose::AdjustCurrentWeaponMenuRotation',
	'VRPose::SaveCurrentWeaponMenuAdjust',
	'VRPose::AdjustCurrentLeftHandMenuPosition',
	'VRPose::AdjustCurrentLeftHandMenuRotation',
	'VRPose::SaveCurrentLeftHandMenuAdjust'
)) {
	if (!$menuSource.Contains($required)) {
		throw "Missing Advanced offset navigation: $required"
	}
}
foreach ($required in @(
	'IsWeaponBodyRecentlySeen(2) && sActiveWeaponProfileKey >= 0',
	'SaveWeaponAdjustForMeshes(meshes, sActiveWeaponProfileKey)',
	'forcedWeaponKey >= 0 ? forcedWeaponKey : sCurrentWeaponKey',
	'return SaveLeftHandAdjust();'
)) {
	if (!$poseSource.Contains($required)) {
		throw "Missing safe Advanced offset persistence: $required"
	}
}
foreach ($required in @(
	'if (VRMenu::IsOpen()) {',
	'InterlockedExchange(&sPadButtons, 0);',
	'InterlockedExchange(&sWeaponInventoryHeld, 0);',
	'InterlockedExchange(&sPadFollowRX, 0);',
	'InterlockedExchange(&sPadFollowRY, 0);'
)) {
	if (!$poseSource.Contains($required)) {
		throw "Missing VR-menu held-input release: $required"
	}
}
foreach ($required in @(
	'bool SaveCurrentWeaponMenuAdjust();',
	'bool SaveCurrentLeftHandMenuAdjust();'
)) {
	if (!$poseHeader.Contains($required)) {
		throw "Missing Advanced offset API: $required"
	}
}

foreach ($required in @(
	'checkAt("SCOPE FORCED ADS",-0.66f,ys[3],ms.scopeForcedADS,true)'
)) {
    if (!$drawSource.Contains($required)) {
        throw "Missing Scope Forced ADS menu behavior: $required"
    }
}
foreach ($required in @(
	'return sSelectedRow == 4;',
	'sSelectedRow == 2 || sSelectedRow == 3 ||',
	's.resolutionPreset = max(0, min(4, s.resolutionPreset + (direction > 0 ? 1 : -1)));',
    's.scopeSensitivityMeters + direction * 0.01f',
    's.scopeSensitivityMeters = 0.05f',
    'ReadFloat(f, "scope_sensitivity_meters", &sSettings.scopeSensitivityMeters)',
    'sSettings.scopeSensitivityMeters = defaults.scopeSensitivityMeters'
)) {
    if (!$menuSource.Contains($required)) {
        throw "Missing Scope Sensitivity menu behavior: $required"
    }
}
if (!$menuHeader.Contains('float scopeSensitivityMeters;')) {
    throw 'Scope Sensitivity setting is missing.'
}
if (!$drawSource.Contains('ms.scopeSensitivityMeters,0.01f,0.30f,true')) {
    throw 'Scope Sensitivity slider is not rendering its live value.'
}
foreach ($required in @(
    'const float engageHeight = VRMenu::GetSettings().scopeSensitivityMeters;',
    'const float releaseHeight = engageHeight + kAimReleaseHysteresisMeters;',
	'kAimGripBelowEyeMeters = 0.08f;',
	'kAimReleaseHysteresisMeters = 0.02f;',
	'kScopeGripToEyeBaseMeters = 0.25f;',
	'fabsf(dy + kAimGripBelowEyeMeters)',
	'sqrtf(dx * dx + height * height + dz * dz)',
	'engageScopeDistance = kScopeGripToEyeBaseMeters + engageHeight;',
	'sWeaponNearFace = (height < releaseHeight);',
	'sWeaponNearFace = (height < engageHeight);',
	'sScopeNearEye = (height < releaseHeight',
	'scopeDistance < releaseScopeDistance',
	'sScopeNearEye = (height < engageHeight',
	'scopeDistance < engageScopeDistance'
)) {
    if (!$poseSource.Contains($required)) {
        throw "Missing Scope Sensitivity runtime behavior: $required"
    }
}
foreach ($removedDiagnostic in @(
	'kADSHandHeightTrace',
	'VRPose ads: alignment-error'
)) {
	if ($poseSource.Contains($removedDiagnostic)) {
		throw "Scope tuning diagnostic remains in release source: $removedDiagnostic"
	}
}
foreach ($required in @(
	'const bool wantScopeZoom = IsScopeNearEye() && (known2x || known4x);',
    'wantScopeZoom ? (known4x ? 3488 : 1744) : 1000',
    'VRMenu::GetSettings().scopeForcedADS'
)) {
    if (!$poseSource.Contains($required)) {
        throw "Missing Scope Forced ADS runtime behavior: $required"
    }
}

foreach ($required in @(
	'bool PresentMenuOverlay(ID3D11Texture2D *texture)',
	'"metro2033reduxvr.settings_menu"',
	'SetOverlayTransformTrackedDeviceRelative(sMenuOverlay',
	'SetOverlayTexture(sMenuOverlay',
	'HideMenuOverlay();'
)) {
	if (!$poseSource.Contains($required)) {
		throw "Missing compositor-owned VR menu overlay: $required"
	}
}

foreach ($required in @(
    'checkAt("AMMO COUNTER",-0.66f,ys[4],ms.ammoCounterEnabled,true)',
    'isAmmoDigitDraw && !VRMenu::GetSettings().ammoCounterEnabled',
    'sAmmoHUDLastConfirmedFrame = G->frame_no',
    'G->frame_no - sAmmoHUDLastConfirmedFrame <= 1',
    'VRMenu::GetSettings().ammoCounterEnabled || !vs || !ps || drawCount != 6',
    'sAmmoBlockArmed && vsHash == kUI2DSpriteVS && psHash == kReticlePS'
)) {
    if (!$drawSource.Contains($required)) { throw "Missing Ammo Counter behavior: $required" }
}
foreach ($forbidden in @(
    'static const bool kHideAmmoPanels = true',
    'static const bool kHideAmmoMagIcon = true',
    'static const bool kHideAmmoKnifeIcon = true'
)) {
    if ($drawSource.Contains($forbidden)) { throw "Unsafe shader-wide ammo suppression returned: $forbidden" }
}

$drawStart = $drawSource.IndexOf('void HackerContext::DrawVRMenu()')
$drawEnd = $drawSource.IndexOf('void HackerContext::DrawWatchDisplay()', $drawStart)
if ($drawStart -lt 0 -or $drawEnd -lt 0) { throw 'Could not isolate DrawVRMenu.' }
$drawBody = $drawSource.Substring($drawStart, $drawEnd - $drawStart)
if ($drawBody.Contains('VRPose::IsGameplayModeActive()')) {
    throw 'VR menu rendering still depends on the gameplay latch.'
}

if (!$menuHeader.Contains('bool expandedVisibility;')) {
    throw 'Expanded Visibility setting is missing.'
}
if (!$visibilitySource.Contains('VRMenu::ExpandedVisibilityActive()')) {
	throw 'Applied Expanded Visibility mode does not gate the v26 coverage envelope.'
}
foreach ($required in @(
	'sAppliedExpandedVisibility = sSettings.expandedVisibility;',
	'bool ExpandedVisibilityActive()',
	'bool ExpandedVisibilityChangePending()',
	'RESTART METRO TO APPLY'
)) {
	if (!$menuSource.Contains($required) -and !$drawSource.Contains($required)) {
		throw "Expanded Visibility launch-time state is incomplete: $required"
	}
}
$wideStart = $poseSource.IndexOf('void ForceWideCullingFov()')
$wideGate = $poseSource.IndexOf('if (!VRMenu::ExpandedVisibilityActive())', $wideStart)
$wideCpu = $poseSource.IndexOf('InstallCpuScreenOcclusionBypass();', $wideStart)
$wideObject = $poseSource.IndexOf('InstallObjectFrustumBypass();', $wideStart)
$wideCluster = $poseSource.IndexOf('InstallClusterFrustumBypass();', $wideStart)
if ($wideStart -lt 0 -or $wideGate -lt $wideStart -or
	$wideCpu -lt $wideGate -or $wideObject -lt $wideGate -or
	$wideCluster -lt $wideGate -or $wideCluster - $wideStart -gt 1200) {
	throw 'Expanded Visibility does not gate all broad culling bypasses.'
}
if (!$poseSource.Contains('wanted[kVisOcclusion] = StereoSinglePass::FlagFilePresent(L"vr_vis_occlusion_bypass_off.txt") ? 0 : 1;')) {
	throw 'CPU screen-occlusion bypass is not the default Expanded Visibility policy.'
}
if ($poseSource.Contains('wanted[kVisOcclusion] = StereoSinglePass::FlagFilePresent(L"vr_vis_occlusion_bypass_on.txt") ? 1 : 0;')) {
	throw 'CPU screen-occlusion bypass unexpectedly requires the rejected opt-in policy.'
}
foreach ($forbidden in @('InstallCpuScreenOcclusionBypass', 'sUpstreamCameraYieldingToScript =', 'NativeStateFollowSelected() =')) {
    if ($visibilitySource.Contains($forbidden)) {
        throw "Expanded Visibility crossed its documented boundary: $forbidden"
    }
}

Write-Host 'VR menu revamp verification passed.'
