param([string]$Dll = "$PSScriptRoot\..\ThirdParty\3Dmigoto\builds\x64\Release\d3d11.dll")
$ErrorActionPreference = 'Stop'
# Binary smoke check, not proof that every reachable path is diagnostic-free.
# The retired hot-path reports must not be present in the Release artifact.
$playBytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Dll).Path)
$playText = [System.Text.Encoding]::ASCII.GetString($playBytes)
$retiredReports = @(
    'VRPose camera-credit integration: matches=',
    'VRPose camera-credit ownership example:',
    'VRPose camera-credit misses:',
    'VRPose camera-credit miss example:',
    'VRPose native ownership observer: camera=',
    'VRPose aimcull: rendered-vs-engine',
    'VRPose watch signature: row0=',
    'VRPose watch UI: timer-atlas fallback active',
    'VRPose watch UI: HH:MM placed on exact casing face',
    'VRPose gas-mask visual state:',
    'VRPose %swatch: native child reparented',
    'VRPose diag: xform summary',
    'StereoTwin RT: engine changed render targets',
    'StereoTwin PERF:',
    'StereoTwin RUNS:',
    'video_memory phase=',
    'stereo_policy phase=',
    'render_path_summary frame=',
    'openvr_frame_timing frame_index=',
    'd3d11 feature_level='
)
foreach ($playMarker in $retiredReports) {
    if ($playText.Contains($playMarker)) { throw "Retired diagnostic remains: $playMarker" }
}
foreach ($playMarker in @(
    'VRPose native body turn: direct +28B280 input delivery ready',
    'VRPose camera-credit integration: generation/copy/command/replay adapters installed'
)) {
    if (!$playText.Contains($playMarker)) { throw "Required installation marker missing: $playMarker" }
}
Write-Output "PASS: $($retiredReports.Count) retired reports absent; native installation markers retained."
Get-FileHash -LiteralPath $Dll -Algorithm SHA256
