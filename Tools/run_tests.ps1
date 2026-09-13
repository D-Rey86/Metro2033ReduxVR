[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'run_cpp_tests.ps1')

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    throw 'Python 3 was not found on PATH.'
}

$pythonTests = @(
    'test_decal_bounded_depth_offset.py',
    'test_embedded_weapon_calibrations.py',
    'test_openvr_frame_pacing.py',
    'test_openvr_resolution_contract.py',
    'test_vr_menu_compositor_overlay.py'
)
foreach ($test in $pythonTests) {
    & $python.Source (Join-Path $PSScriptRoot $test)
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $test" }
}

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'test_store_discovery.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Test failed: test_store_discovery.ps1' }

Write-Host 'All self-contained public-source tests passed.'

