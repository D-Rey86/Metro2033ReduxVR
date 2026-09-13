[CmdletBinding()]
param(
    [string]$GamePath,
    [AllowNull()][string]$SteamRoot = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath,
    [AllowNull()][string]$EpicManifestRoot = $(if ($env:ProgramData) { Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests' }),
    [switch]$NonInteractive
)

$ErrorActionPreference = 'Stop'
$expectedExeSha256 = '183EF65212E351C55A2832C3F3C8B04616B153697913D33B1E27683163F14E15'
$backupRoot = $null
$installed = New-Object System.Collections.Generic.List[object]
$qualityChanged = $false
$qualityConfigExisted = $false
$qualitySettingExisted = $false
$previousQualityValue = $null
$qualityBackupPath = $null

. (Join-Path $PSScriptRoot 'StoreDiscovery.ps1')

function Find-MetroPath {
    $candidates = @(Get-MetroGameCandidates -SteamRoot $SteamRoot -EpicManifestRoot $EpicManifestRoot)
    foreach ($candidate in $candidates) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $candidate.Path 'metro.exe') -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($hash -eq $expectedExeSha256) { return $candidate.Path }
    }
    if ($candidates.Count -gt 0) { return $candidates[0].Path }
    return $null
}

function Stop-WithMessage([string]$message) {
    Write-Host ''
    Write-Host $message -ForegroundColor Red
    if (-not $NonInteractive) {
        Write-Host ''
        Read-Host 'Press Enter to close' | Out-Null
    }
    exit 1
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

function Set-MetroQuality([string]$path, [string]$value) {
    $linePattern = '(?m)^[ \t]*r_quality_level[ \t]+([^\s\r\n]+)[ \t]*(?=\r?$)'
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $document = Read-TextFile $path
        $text = $document.Text
        $encoding = $document.Encoding
    }
    else {
        $text = ''
        $encoding = New-Object Text.UTF8Encoding($false)
    }

    $matches = [regex]::Matches($text, $linePattern)
    if ($matches.Count -gt 1) {
        throw 'user.cfg contains more than one r_quality_level entry; no quality setting was changed.'
    }

    $script:qualitySettingExisted = $matches.Count -eq 1
    if ($script:qualitySettingExisted) {
        $script:previousQualityValue = $matches[0].Groups[1].Value
        $updated = [regex]::Replace($text, $linePattern, "r_quality_level $value")
    }
    else {
        $newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
        $prefix = if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) { $newline } else { '' }
        $updated = $text + $prefix + "r_quality_level $value" + $newline
    }

    [IO.File]::WriteAllText($path, $updated, $encoding)
}

try {
    if (Get-Process -Name metro -ErrorAction SilentlyContinue) {
        Stop-WithMessage 'Metro 2033 Redux is running. Close the game, then run the installer again.'
    }

    if (-not $GamePath) {
        $GamePath = Find-MetroPath
    }
    if (-not $GamePath -and -not $NonInteractive) {
        $GamePath = Read-Host 'Metro was not found automatically. Enter the folder containing metro.exe'
    }
    if (-not $GamePath) {
        Stop-WithMessage 'Metro 2033 Redux could not be found.'
    }

    $GamePath = [IO.Path]::GetFullPath($GamePath.Trim('"'))
    $metroExe = Join-Path $GamePath 'metro.exe'
    if (-not (Test-Path -LiteralPath $metroExe -PathType Leaf)) {
        Stop-WithMessage "metro.exe was not found in: $GamePath"
    }
    $existingRecord = Join-Path $GamePath 'Metro2033ReduxVR.install.json'
    if (Test-Path -LiteralPath $existingRecord -PathType Leaf) {
        Stop-WithMessage 'Metro2033ReduxVR is already recorded as installed. Run Uninstall.cmd before installing this build.'
    }

    $actualExeSha256 = (Get-FileHash -LiteralPath $metroExe -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actualExeSha256 -ne $expectedExeSha256) {
        $message = "This Metro executable has not been tested.`nExpected: $expectedExeSha256`nFound:    $actualExeSha256"
        if ($NonInteractive) {
            Stop-WithMessage $message
        }
        Write-Host $message -ForegroundColor Yellow
        $answer = Read-Host 'Continue anyway? Type YES to continue'
        if ($answer -cne 'YES') {
            Stop-WithMessage 'Installation cancelled without changing the game folder.'
        }
    }

    $payloadRoot = Join-Path $PSScriptRoot 'Payload'
    $payloadManifestPath = Join-Path $PSScriptRoot 'payload-manifest.json'
    if (-not (Test-Path -LiteralPath $payloadManifestPath)) {
        Stop-WithMessage 'payload-manifest.json is missing. Re-extract the complete tester package.'
    }
    $payloadManifest = Get-Content -LiteralPath $payloadManifestPath -Raw | ConvertFrom-Json

    foreach ($file in $payloadManifest.files) {
        $source = Join-Path $payloadRoot $file.path
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            Stop-WithMessage "Package payload is incomplete: $($file.path) is missing."
        }
        $actual = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($actual -ne $file.sha256.ToUpperInvariant()) {
            Stop-WithMessage "Package integrity check failed for $($file.path). Re-download or re-copy the ZIP."
        }
    }

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backupRoot = Join-Path $GamePath "Metro2033ReduxVR_Backups\$stamp"
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null

    $qualityConfigPath = Join-Path $GamePath 'user.cfg'
    $qualityConfigExisted = Test-Path -LiteralPath $qualityConfigPath -PathType Leaf
    if ($qualityConfigExisted) {
        $qualityBackupPath = Join-Path $backupRoot 'user.cfg.pre-install'
        Copy-Item -LiteralPath $qualityConfigPath -Destination $qualityBackupPath -Force
    }

    foreach ($file in $payloadManifest.files) {
        $relative = [string]$file.path
        $source = Join-Path $payloadRoot $relative
        $target = Join-Path $GamePath $relative
        $targetDirectory = Split-Path -Parent $target
        New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null

        $existed = Test-Path -LiteralPath $target -PathType Leaf
        if ($existed) {
            $backup = Join-Path $backupRoot $relative
            New-Item -ItemType Directory -Path (Split-Path -Parent $backup) -Force | Out-Null
            Copy-Item -LiteralPath $target -Destination $backup -Force
        }
        $installed.Add([pscustomobject]@{
            path = $relative
            sha256 = $file.sha256.ToUpperInvariant()
            existedBefore = $existed
        })
        Copy-Item -LiteralPath $source -Destination $target -Force
        $installedHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($installedHash -ne $file.sha256.ToUpperInvariant()) {
            throw "Installed-file verification failed for $relative"
        }
    }

    # The validated VR routes depend on Metro's Medium shader package. Apply it
    # before the first launch so an inherited Very High profile cannot make the
    # opening 3D menu unusably slow or load incompatible shader variants.
    $qualityChanged = $true
    Set-MetroQuality -path $qualityConfigPath -value '3'
    $installedQuality = [regex]::Match(
        (Read-TextFile $qualityConfigPath).Text,
        '(?m)^[ \t]*r_quality_level[ \t]+([^\s\r\n]+)[ \t]*(?=\r?$)')
    if (-not $installedQuality.Success -or $installedQuality.Groups[1].Value -ne '3') {
        throw 'Medium-quality verification failed after updating user.cfg.'
    }

    $installRecord = [ordered]@{
        product = 'Metro2033ReduxVR'
        release = $payloadManifest.release
        sourceCommit = $payloadManifest.sourceCommit
        installedAt = (Get-Date).ToString('o')
        gamePath = $GamePath
        gameExeSha256 = $actualExeSha256
        backupDirectory = $backupRoot
        qualitySetting = [ordered]@{
            configPath = 'user.cfg'
            configExistedBefore = $qualityConfigExisted
            settingExistedBefore = $qualitySettingExisted
            previousValue = $previousQualityValue
            installedValue = '3'
        }
        files = $installed
    }
    $recordPath = Join-Path $GamePath 'Metro2033ReduxVR.install.json'
    $installRecord | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $recordPath -Encoding UTF8

    Write-Host ''
    Write-Host 'Metro2033ReduxVR installed successfully.' -ForegroundColor Green
    Write-Host "Game folder: $GamePath"
    Write-Host "Backup:     $backupRoot"
    Write-Host 'Metro quality: Medium (required by this VR build)'
    Write-Host ''
    Write-Host 'Start SteamVR first, then launch Metro 2033 Redux normally through your game launcher.'
    Write-Host 'Hold both stick clicks for about one second to open the VR menu.'
    if (-not $NonInteractive) {
        Write-Host ''
        Read-Host 'Press Enter to close' | Out-Null
    }
}
catch {
    $failure = $_.Exception.Message
    if ($qualityChanged -and $GamePath) {
        $qualityConfigPath = Join-Path $GamePath 'user.cfg'
        if ($qualityConfigExisted -and $qualityBackupPath -and
            (Test-Path -LiteralPath $qualityBackupPath -PathType Leaf)) {
            Copy-Item -LiteralPath $qualityBackupPath -Destination $qualityConfigPath -Force
        }
        elseif (-not $qualityConfigExisted -and
            (Test-Path -LiteralPath $qualityConfigPath -PathType Leaf)) {
            Remove-Item -LiteralPath $qualityConfigPath -Force
        }
    }
    if ($backupRoot -and $GamePath -and $installed.Count -gt 0) {
        for ($i = $installed.Count - 1; $i -ge 0; --$i) {
            $file = $installed[$i]
            $target = Join-Path $GamePath ([string]$file.path)
            $backup = Join-Path $backupRoot ([string]$file.path)
            if ($file.existedBefore -and (Test-Path -LiteralPath $backup -PathType Leaf)) {
                Copy-Item -LiteralPath $backup -Destination $target -Force
            }
            elseif (Test-Path -LiteralPath $target -PathType Leaf) {
                Remove-Item -LiteralPath $target -Force
            }
        }
        Write-Host 'Partial installation was rolled back. Its recovery backup was retained.' -ForegroundColor Yellow
    }
    Stop-WithMessage "Installation failed: $failure"
}
