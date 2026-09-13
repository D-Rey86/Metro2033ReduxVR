[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $repoRoot 'Packaging\Tester\StoreDiscovery.ps1')

function Assert-True([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('Metro2033ReduxVR-store-test-' + [guid]::NewGuid().ToString('N'))
$resolvedTemp = [IO.Path]::GetFullPath($testRoot)
Assert-True ($resolvedTemp.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) 'Unsafe temporary test path.'

try {
    $steamRoot = Join-Path $testRoot 'Steam'
    $steamLibrary = Join-Path $testRoot 'SteamLibrary'
    $steamGame = Join-Path $steamLibrary 'steamapps\common\Metro 2033 Redux'
    $epicRoot = Join-Path $testRoot 'EpicManifests'
    $epicGame = Join-Path $testRoot 'EpicGames\Metro2033Redux'
    New-Item -ItemType Directory -Path (Join-Path $steamRoot 'steamapps'),$steamGame,$epicRoot,$epicGame -Force | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $steamGame 'metro.exe'), [byte[]](1,2,3))
    [IO.File]::WriteAllText((Join-Path $steamRoot 'steamapps\libraryfolders.vdf'), "`"path`" `"$($steamLibrary -replace '\\','\\')`"`r`n")
    [IO.File]::WriteAllBytes((Join-Path $epicGame 'metro.exe'), [byte[]](4,5,6))
    [ordered]@{
        DisplayName = 'Metro 2033 Redux'
        AppName = 'Metro2033Redux'
        InstallLocation = $epicGame
        LaunchExecutable = 'metro.exe'
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $epicRoot 'metro.item') -Encoding UTF8

    $found = @(Get-MetroGameCandidates -SteamRoot $steamRoot -EpicManifestRoot $epicRoot)
    Assert-True ($found.Count -eq 2) 'Expected one Steam and one Epic candidate.'
    Assert-True (($found | Where-Object Store -eq 'Steam').Path -eq [IO.Path]::GetFullPath($steamGame)) 'Steam library discovery failed.'
    Assert-True (($found | Where-Object Store -eq 'Epic Games Store').Path -eq [IO.Path]::GetFullPath($epicGame)) 'Epic manifest discovery failed.'

    [ordered]@{
        DisplayName = 'Not Metro'
        InstallLocation = (Join-Path $testRoot 'WrongGame')
        LaunchExecutable = 'metro.exe'
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $epicRoot 'wrong.item') -Encoding UTF8
    $foundAgain = @(Get-MetroGameCandidates -SteamRoot $steamRoot -EpicManifestRoot $epicRoot)
    Assert-True ($foundAgain.Count -eq 2) 'An unrelated Epic manifest was accepted.'

    Write-Output 'Steam and Epic Metro discovery tests passed.'
}
finally {
    if (Test-Path -LiteralPath $resolvedTemp) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
