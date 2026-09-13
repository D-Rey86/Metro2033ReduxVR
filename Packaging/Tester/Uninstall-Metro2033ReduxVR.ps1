[CmdletBinding()]
param(
    [string]$GamePath,
    [AllowNull()][string]$SteamRoot = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath,
    [AllowNull()][string]$EpicManifestRoot = $(if ($env:ProgramData) { Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests' }),
    [switch]$NonInteractive
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'StoreDiscovery.ps1')

function Find-InstalledMetroPath {
    foreach ($candidate in @(Get-MetroGameCandidates -SteamRoot $SteamRoot -EpicManifestRoot $EpicManifestRoot)) {
        if (Test-Path -LiteralPath (Join-Path $candidate.Path 'Metro2033ReduxVR.install.json')) {
            return $candidate.Path
        }
    }
    return $null
}

function Finish([string]$message, [ConsoleColor]$color, [int]$code) {
    Write-Host ''
    Write-Host $message -ForegroundColor $color
    if (-not $NonInteractive) {
        Write-Host ''
        Read-Host 'Press Enter to close' | Out-Null
    }
    exit $code
}

function Read-TextFile([string]$path) {
    $reader = New-Object IO.StreamReader($path, [Text.Encoding]::Default, $true)
    try {
        $text = $reader.ReadToEnd()
        return [pscustomobject]@{ Text = $text; Encoding = $reader.CurrentEncoding }
    }
    finally {
        $reader.Dispose()
    }
}

function Restore-MetroQuality([string]$path, $quality, $warnings) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        if ([bool]$quality.configExistedBefore) {
            $warnings.Add('Could not restore the previous Metro quality: user.cfg is missing.')
        }
        return
    }

    $document = Read-TextFile $path
    $linePattern = '(?m)^[ \t]*r_quality_level[ \t]+([^\s\r\n]+)[ \t]*(?=\r?$)'
    $matches = [regex]::Matches($document.Text, $linePattern)
    if ($matches.Count -ne 1) {
        $warnings.Add('Kept user.cfg quality unchanged because its r_quality_level entry is missing or ambiguous.')
        return
    }
    if ($matches[0].Groups[1].Value -ne [string]$quality.installedValue) {
        $warnings.Add('Kept the current Metro quality because it was changed after installing the mod.')
        return
    }

    if ([bool]$quality.settingExistedBefore) {
        $updated = [regex]::Replace(
            $document.Text, $linePattern,
            "r_quality_level $([string]$quality.previousValue)")
    }
    else {
        $removePattern = '(?m)^[ \t]*r_quality_level[ \t]+[^\r\n]*(?:\r?\n|$)'
        $updated = (New-Object Text.RegularExpressions.Regex($removePattern)).Replace(
            $document.Text, '', 1)
    }
    [IO.File]::WriteAllText($path, $updated, $document.Encoding)
}

try {
    if (Get-Process -Name metro -ErrorAction SilentlyContinue) {
        Finish 'Metro 2033 Redux is running. Close the game, then run the uninstaller again.' Red 1
    }
    if (-not $GamePath) {
        $GamePath = Find-InstalledMetroPath
    }
    if (-not $GamePath -and -not $NonInteractive) {
        $GamePath = Read-Host 'Enter the Metro folder containing Metro2033ReduxVR.install.json'
    }
    if (-not $GamePath) {
        Finish 'An installed copy of Metro2033ReduxVR could not be found.' Red 1
    }

    $GamePath = [IO.Path]::GetFullPath($GamePath.Trim('"'))
    $recordPath = Join-Path $GamePath 'Metro2033ReduxVR.install.json'
    if (-not (Test-Path -LiteralPath $recordPath -PathType Leaf)) {
        Finish "The install record was not found in: $GamePath" Red 1
    }
    $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
    $warnings = New-Object System.Collections.Generic.List[string]

    foreach ($file in $record.files) {
        $target = Join-Path $GamePath ([string]$file.path)
        $backup = Join-Path ([string]$record.backupDirectory) ([string]$file.path)
        $safeToReplace = $true
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            $currentHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToUpperInvariant()
            if ($currentHash -ne $file.sha256.ToUpperInvariant()) {
                $safeToReplace = $false
                $warnings.Add("Kept modified file: $($file.path)")
            }
        }
        if ($safeToReplace) {
            if (Test-Path -LiteralPath $target -PathType Leaf) {
                Remove-Item -LiteralPath $target -Force
            }
            if ($file.existedBefore -and (Test-Path -LiteralPath $backup -PathType Leaf)) {
                New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
                Copy-Item -LiteralPath $backup -Destination $target -Force
            }
        }
    }

    if ($record.PSObject.Properties.Name -contains 'qualitySetting') {
        $qualityPath = Join-Path $GamePath ([string]$record.qualitySetting.configPath)
        Restore-MetroQuality -path $qualityPath -quality $record.qualitySetting -warnings $warnings
    }

    Remove-Item -LiteralPath $recordPath -Force
    Write-Host ''
    Write-Host 'Metro2033ReduxVR was uninstalled.' -ForegroundColor Green
    Write-Host "The recoverable backup was retained at: $($record.backupDirectory)"
    foreach ($warning in $warnings) {
        Write-Host $warning -ForegroundColor Yellow
    }
    if (-not $NonInteractive) {
        Write-Host ''
        Read-Host 'Press Enter to close' | Out-Null
    }
}
catch {
    Finish "Uninstall failed: $($_.Exception.Message)" Red 1
}
