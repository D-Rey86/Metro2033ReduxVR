[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $repositoryRoot 'Builds\cpp-tests'
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}
$installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installationPath) {
    throw 'A Visual Studio installation with the x64 C++ tools was not found.'
}
$developerShell = Join-Path $installationPath 'Common7\Tools\Launch-VsDevShell.ps1'
& $developerShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$tests = @(
    @{ Source = 'test_native_visibility_coverage.cpp'; Extra = @() },
    @{ Source = 'test_native_visibility_adapter.cpp'; Extra = @('/wd4505') },
    @{ Source = 'test_native_camera_command_credits.cpp'; Extra = @() },
    @{ Source = 'test_native_camera_credit_adapter.cpp'; Extra = @('/wd4505') },
    @{ Source = 'test_native_body_turn.cpp'; Extra = @() },
    @{ Source = 'test_native_vendor_camera_owner.cpp'; Extra = @('/wd4505') },
    @{ Source = 'test_monitor_independent_resolution.cpp'; Extra = @() },
    @{ Source = 'test_vr_compatibility.cpp'; Extra = @() }
)

foreach ($test in $tests) {
    $sourcePath = Join-Path $PSScriptRoot $test.Source
    $baseName = [IO.Path]::GetFileNameWithoutExtension($test.Source)
    $objectPath = Join-Path $outputRoot "$baseName.obj"
    $executablePath = Join-Path $outputRoot "$baseName.exe"
    $arguments = @('/nologo', '/O2', '/EHsc', '/W4', '/WX') + $test.Extra + @($sourcePath, "/Fo$objectPath", "/Fe$executablePath")
    & cl.exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($test.Source)" }
    & $executablePath
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $($test.Source)" }
}

Write-Host "All $($tests.Count) C++ tests passed."

