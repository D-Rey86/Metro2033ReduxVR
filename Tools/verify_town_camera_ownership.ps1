$ErrorActionPreference = 'Stop'
$sourcePath = "$PSScriptRoot\..\ThirdParty\3Dmigoto\DirectX11\VRPose.cpp"
$source = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $sourcePath).Path)

# The raw player-performance classifier may feed only the native-gated camera
# owner and hand routing. Camera/input consumers must use the combined native-
# gate result, never the raw `(1,1)` performance state.
$occurrences = ([regex]::Matches($source, 'ScriptedPlayerPerformanceActive\(\)')).Count
if ($occurrences -ne 4) {
    throw "Raw player-performance state escaped its two classifiers: found $occurrences references, expected 4."
}
$ownerStart = $source.IndexOf('static bool ScriptedPlayerCameraOwnerActive()')
$ownerStart = $source.IndexOf('static bool ScriptedPlayerCameraOwnerActive()', $ownerStart + 1)
$ownerEnd = $source.IndexOf('bool NativeScriptedViewmodelActive()', $ownerStart)
if ($ownerStart -lt 0 -or $ownerEnd -lt 0) {
    throw 'Native-gated player camera owner definition is missing.'
}
$ownerBody = $source.Substring($ownerStart, $ownerEnd - $ownerStart)
foreach ($required in @(
    'ScriptedPlayerPerformanceActive()',
    '!MetroNativeLookInputAllowed((BYTE *)player)')) {
    if (!$ownerBody.Contains($required)) {
        throw "Player camera owner is missing native distinction: $required"
    }
}
$handStart = $source.IndexOf('bool ScriptedHandPerformanceActive()')
$handEnd = $source.IndexOf('LONG CALLBACK CameraWriteProbeVeh', $handStart)
if ($handStart -lt 0 -or $handEnd -lt 0 -or
    !$source.Substring($handStart, $handEnd - $handStart).Contains(
        'ScriptedPlayerPerformanceActive()')) {
    throw 'Player-performance classifier is no longer retained by scripted hand routing.'
}
foreach ($consumer in @(
    'NativeFollowInputAllowed',
    'Hooked_UpstreamCameraBasis',
    'Hooked_DirectMovementHeading')) {
    $start = $source.IndexOf($consumer)
    if ($start -lt 0) { throw "Missing camera/input consumer: $consumer" }
    $window = $source.Substring($start, [Math]::Min(8000, $source.Length - $start))
    if (!$window.Contains('ScriptedPlayerCameraOwnerActive()')) {
        throw "Camera/input consumer lacks native-gated performance ownership: $consumer"
    }
}
foreach ($removed in @(
    'VRPose authored gate diagnostic:',
    'sObservedAuthoredCameraFlag',
    'TraceAuthoredGateSources')) {
    if ($source.Contains($removed)) {
        throw "Temporary town ownership diagnostic remains: $removed"
    }
}

Write-Output 'PASS: raw player-performance state is restricted to native-gated camera and hand routing; temporary diagnostics removed.'
