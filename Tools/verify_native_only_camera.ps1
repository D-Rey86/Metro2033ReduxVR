param([string]$Dll = "$PSScriptRoot\..\ThirdParty\3Dmigoto\builds\x64\Release\d3d11.dll")
$ErrorActionPreference = 'Stop'
$nativeOnlySource = [IO.File]::ReadAllText("$PSScriptRoot\..\ThirdParty\3Dmigoto\DirectX11\VRPose.cpp")
foreach ($removed in @('SendLookAxes(', 'SendLookInput(', 'ToggleUpstreamCameraBasisProbeController(', 'ToggleDirectCameraConstructionProbeController(', 'sPendingTurnYaw += dx')) {
    if ($nativeOnlySource.Contains($removed)) { throw "Retired camera delivery remains: $removed" }
}
if ($nativeOnlySource -notmatch 'sUpstreamCameraBasisProbeActive = 1;') { throw 'Native mode must start selected.' }
if ($nativeOnlySource -match 'InterlockedExchange\(&sUpstreamCameraBasisProbeActive,') { throw 'Runtime mode selection write remains.' }
foreach ($required in @('nativeBodyTurnInput.Publish(dx', 'sNativeFollowSettleFrames = 32;', 'SeedNativeFollowHistory();', 'InstallNativeCameraCreditAdapter();', 'InstallNativeVendorCameraOwner();', 'InstallNativeVisibilityCoverage();')) {
    if (!$nativeOnlySource.Contains($required)) { throw "Required native path missing: $required" }
}
$vendorSource = [IO.File]::ReadAllText("$PSScriptRoot\..\ThirdParty\3Dmigoto\DirectX11\NativeVendorCameraOwner.inl")
foreach ($required in @('base + 0x42DDE0', 'base + 0x42DE3C', 'base + 0xB7A8D8', 'base + 0xB7A8E0')) {
    if (!$vendorSource.Contains($required)) { throw "Required vendor owner signature missing: $required" }
}
$samplerSource = [IO.File]::ReadAllText("$PSScriptRoot\observe_native_look_state.py")
foreach ($removed in @('first native trade activation observed', 'first native trade deactivation observed')) {
    if ($vendorSource.Contains($removed)) { throw "Temporary vendor diagnostic remains: $removed" }
}
if ($samplerSource.Contains('--vendor-owner-code')) { throw 'Temporary vendor code sampler remains.' }
$nativeOnlyBinary = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Dll).Path))
if (!$nativeOnlyBinary.Contains('native-only, no mouse fallback or mode toggle')) { throw 'Native-only installation marker missing.' }
if (!$nativeOnlyBinary.Contains('vendor camera owner: %s at trade/customize')) { throw 'Vendor-owner installation marker missing.' }
foreach ($removed in @('starts in legacy', 'inactive - legacy mouse follow', 'both controller grips toggle direct HMD follow')) {
    if ($nativeOnlyBinary.Contains($removed)) { throw "Retired runtime mode marker remains: $removed" }
}
foreach ($removed in @('first native trade activation observed', 'first native trade deactivation observed')) {
    if ($nativeOnlyBinary.Contains($removed)) { throw "Temporary vendor diagnostic marker remains: $removed" }
}
& "$PSScriptRoot\verify_town_camera_ownership.ps1"
& "$PSScriptRoot\verify_native_camera_play_build.ps1" -Dll $Dll
Write-Output 'PASS: native-only source/delivery and Release marker guards. Startup/headset behavior still requires live validation.'
