[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,
    [Parameter(Mandatory = $true)]
    [string]$MetroExe
)

$ErrorActionPreference = 'Stop'

function Assert-True([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}

function Read-All([string]$path) {
    return [IO.File]::ReadAllText($path)
}

function Assert-Setting([string]$text, [string]$key, [string]$value, [string]$message) {
    $pattern = "(?m)^[ \t]*$([regex]::Escape($key))[ \t]+$([regex]::Escape($value))[ \t]*(?=\r?$)"
    Assert-True ([regex]::Matches($text, $pattern).Count -eq 1) $message
}

$PackageRoot = [IO.Path]::GetFullPath($PackageRoot)
$MetroExe = [IO.Path]::GetFullPath($MetroExe)
Assert-True (Test-Path -LiteralPath (Join-Path $PackageRoot 'payload-manifest.json') -PathType Leaf) 'Package manifest is missing.'
Assert-True (Test-Path -LiteralPath $MetroExe -PathType Leaf) 'Metro executable is missing.'

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("Metro2033ReduxVR-installer-test-" + [guid]::NewGuid().ToString('N'))
$resolvedTemp = [IO.Path]::GetFullPath($testRoot)
Assert-True ($resolvedTemp.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) 'Unsafe temporary test path.'

try {
    New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

    # Existing config: installation must set exactly the validated renderer
    # combination. Uninstall restores each unchanged value independently while
    # preserving a later user change and unrelated content.
    $game1 = Join-Path $testRoot 'existing-config'
    New-Item -ItemType Directory -Path $game1 -Force | Out-Null
    Copy-Item -LiteralPath $MetroExe -Destination (Join-Path $game1 'metro.exe')
    $before1 = @(
        'r_quality_level 2'
        'r_dx11_tess 0'
        'r_vsync on'
        'r_supersample 2'
        'r_af_level 0'
        'custom_test_value 10'
        ''
    ) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $game1 'user.cfg'), $before1, (New-Object Text.UTF8Encoding($false)))
    & (Join-Path $PackageRoot 'Install-Metro2033ReduxVR.ps1') -GamePath $game1 -NonInteractive
    $installed1 = Read-All (Join-Path $game1 'user.cfg')
    Assert-Setting $installed1 'r_quality_level' '3' 'Installer did not force Medium quality.'
    Assert-Setting $installed1 'r_dx11_tess' '1' 'Installer did not force Very High tessellation.'
    Assert-Setting $installed1 'r_vsync' 'off' 'Installer did not disable VSync.'
    Assert-Setting $installed1 'r_supersample' '1' 'Installer did not disable SSAA.'
    Assert-Setting $installed1 'r_af_level' '1' 'Installer did not force 16x texture filtering.'
    Assert-True ($installed1 -match '(?m)^custom_test_value 10\r?$') 'Installer changed an unrelated config value.'
    $record1 = Get-Content -LiteralPath (Join-Path $game1 'Metro2033ReduxVR.install.json') -Raw | ConvertFrom-Json
    Assert-True (@($record1.graphicsSettings.settings).Count -eq 5) 'Install record did not capture all five graphics settings.'

    $changed1 = $installed1 -replace '(?m)^r_vsync off\r?$', 'r_vsync on'
    $changed1 = $changed1 -replace 'custom_test_value 10', 'custom_test_value 11'
    [IO.File]::WriteAllText((Join-Path $game1 'user.cfg'), $changed1, (New-Object Text.UTF8Encoding($false)))
    & (Join-Path $PackageRoot 'Uninstall-Metro2033ReduxVR.ps1') -GamePath $game1 -NonInteractive
    $uninstalled1 = Read-All (Join-Path $game1 'user.cfg')
    Assert-Setting $uninstalled1 'r_quality_level' '2' 'Uninstaller did not restore the previous quality.'
    Assert-Setting $uninstalled1 'r_dx11_tess' '0' 'Uninstaller did not restore the previous tessellation.'
    Assert-Setting $uninstalled1 'r_vsync' 'on' 'Uninstaller overwrote a post-install VSync choice.'
    Assert-Setting $uninstalled1 'r_supersample' '2' 'Uninstaller did not restore the previous SSAA value.'
    Assert-Setting $uninstalled1 'r_af_level' '0' 'Uninstaller did not restore the previous texture filtering.'
    Assert-True ($uninstalled1 -match '(?m)^custom_test_value 11\r?$') 'Uninstaller overwrote an unrelated post-install change.'

    # Missing config: remove only installer-owned lines and preserve content
    # created after installation.
    $game2 = Join-Path $testRoot 'missing-config'
    New-Item -ItemType Directory -Path $game2 -Force | Out-Null
    Copy-Item -LiteralPath $MetroExe -Destination (Join-Path $game2 'metro.exe')
    & (Join-Path $PackageRoot 'Install-Metro2033ReduxVR.ps1') -GamePath $game2 -NonInteractive
    [IO.File]::AppendAllText((Join-Path $game2 'user.cfg'), "custom_test_value 7`n", (New-Object Text.UTF8Encoding($false)))
    & (Join-Path $PackageRoot 'Uninstall-Metro2033ReduxVR.ps1') -GamePath $game2 -NonInteractive
    $uninstalled2 = Read-All (Join-Path $game2 'user.cfg')
    foreach ($key in @('r_quality_level', 'r_dx11_tess', 'r_vsync', 'r_supersample', 'r_af_level')) {
        Assert-True ($uninstalled2 -notmatch "(?m)^$([regex]::Escape($key)) ") "Uninstaller retained installer-owned setting $key."
    }
    Assert-True ($uninstalled2 -match '(?m)^custom_test_value 7\r?$') 'Uninstaller removed later unrelated config content.'

    # Automatic Epic discovery must resolve InstallLocation for both install
    # and uninstall.
    $game3 = Join-Path $testRoot 'epic-auto-discovery'
    $epicManifests = Join-Path $testRoot 'epic-manifests'
    $disabledSteam = Join-Path $testRoot 'no-steam-here'
    New-Item -ItemType Directory -Path $game3,$epicManifests -Force | Out-Null
    Copy-Item -LiteralPath $MetroExe -Destination (Join-Path $game3 'metro.exe')
    [ordered]@{
        DisplayName = 'Metro 2033 Redux'
        AppName = 'Metro2033Redux'
        InstallLocation = $game3
        LaunchExecutable = 'metro.exe'
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $epicManifests 'metro.item') -Encoding UTF8
    & (Join-Path $PackageRoot 'Install-Metro2033ReduxVR.ps1') -SteamRoot $disabledSteam -EpicManifestRoot $epicManifests -NonInteractive
    Assert-True (Test-Path -LiteralPath (Join-Path $game3 'Metro2033ReduxVR.install.json')) 'Installer did not use the discovered Epic path.'
    & (Join-Path $PackageRoot 'Uninstall-Metro2033ReduxVR.ps1') -SteamRoot $disabledSteam -EpicManifestRoot $epicManifests -NonInteractive
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $game3 'Metro2033ReduxVR.install.json'))) 'Uninstaller did not find the discovered Epic installation.'

    Write-Output 'Tester installer graphics enforcement, restoration, and Epic discovery tests passed.'
}
finally {
    if (Test-Path -LiteralPath $resolvedTemp) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
