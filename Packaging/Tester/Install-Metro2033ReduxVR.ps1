[CmdletBinding()]
param(
    [string]$GamePath,
    [AllowNull()][string]$SteamRoot = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath,
    [AllowNull()][string]$EpicManifestRoot = $(if ($env:ProgramData) { Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests' }),
    [AllowNull()][string]$LocalAppDataRoot = $env:LOCALAPPDATA,
    [string]$UserConfigPath,
    [switch]$NonInteractive
)

$ErrorActionPreference = 'Stop'
$expectedExeSha256 = '183EF65212E351C55A2832C3F3C8B04616B153697913D33B1E27683163F14E15'
$backupRoot = $null
$installed = New-Object System.Collections.Generic.List[object]
$graphicsConfigChanged = $false
$graphicsConfigExisted = $false
$graphicsBackupPath = $null
$graphicsSettingsState = @()
$requiredGraphicsSettings = @(
    [pscustomobject]@{ Key = 'r_quality_level'; Value = '3'; Name = 'Quality'; DisplayValue = 'Medium' },
    [pscustomobject]@{ Key = 'r_dx11_tess'; Value = '1'; Name = 'Tessellation'; DisplayValue = 'Very High' },
    [pscustomobject]@{ Key = 'r_vsync'; Value = 'off'; Name = 'VSync'; DisplayValue = 'Off' },
    [pscustomobject]@{ Key = 'r_supersample'; Value = '1'; Name = 'SSAA'; DisplayValue = 'Off' },
    [pscustomobject]@{ Key = 'r_af_level'; Value = '1'; Name = 'Texture Filtering'; DisplayValue = '16x' }
)

. (Join-Path $PSScriptRoot 'StoreDiscovery.ps1')

function Find-MetroInstallation {
    $candidates = @(Get-MetroGameCandidates -SteamRoot $SteamRoot -EpicManifestRoot $EpicManifestRoot)
    foreach ($candidate in $candidates) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $candidate.Path 'metro.exe') -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($hash -eq $expectedExeSha256) { return $candidate }
    }
    if ($candidates.Count -gt 0) { return $candidates[0] }
    return $null
}

function Get-MostRecentSteamProfileId {
    if (-not $SteamRoot) { return $null }
    $loginUsersPath = Join-Path $SteamRoot 'config\loginusers.vdf'
    if (-not (Test-Path -LiteralPath $loginUsersPath -PathType Leaf)) { return $null }

    $text = Get-Content -LiteralPath $loginUsersPath -Raw
    $accounts = @([regex]::Matches(
        $text,
        '(?ms)^\s*"(?<id>[0-9]{17})"\s*\{(?<body>.*?)^\s*\}'))
    if ($accounts.Count -eq 0) { return $null }
    $selected = @($accounts | Where-Object {
        $_.Groups['body'].Value -match '(?m)^\s*"MostRecent"\s*"1"\s*$'
    })
    if ($selected.Count -eq 1) {
        $steamId = $selected[0].Groups['id'].Value
    }
    elseif ($accounts.Count -eq 1) {
        $steamId = $accounts[0].Groups['id'].Value
    }
    else {
        return $null
    }

    try { return ([Convert]::ToUInt64($steamId, 10)).ToString('x') }
    catch { return $null }
}

function Resolve-MetroUserConfig([string]$store) {
    if (-not $LocalAppDataRoot) {
        throw 'LOCALAPPDATA is unavailable, so Metro user.cfg cannot be located safely.'
    }
    $profileRoot = Join-Path $LocalAppDataRoot '4A Games\Metro 2033'

    if ($store -eq 'Steam') {
        $steamProfileId = Get-MostRecentSteamProfileId
        if ($steamProfileId) {
            return Join-Path (Join-Path $profileRoot $steamProfileId) 'user.cfg'
        }
    }

    $existing = @()
    if (Test-Path -LiteralPath $profileRoot -PathType Container) {
        $existing = @(Get-ChildItem -LiteralPath $profileRoot -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'user.cfg' } |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
    }
    if ($existing.Count -eq 1) { return $existing[0] }
    if ($existing.Count -gt 1) {
        if ($NonInteractive) {
            throw 'Multiple Metro profiles contain user.cfg; specify UserConfigPath explicitly.'
        }
        Write-Host ''
        Write-Host 'Multiple Metro profiles were found:' -ForegroundColor Yellow
        for ($i = 0; $i -lt $existing.Count; ++$i) {
            Write-Host "[$($i + 1)] $($existing[$i])"
        }
        $selection = Read-Host 'Enter the number of the profile to configure'
        $index = 0
        if (-not [int]::TryParse($selection, [ref]$index) -or
            $index -lt 1 -or $index -gt $existing.Count) {
            throw 'A valid Metro profile was not selected.'
        }
        return $existing[$index - 1]
    }

    throw 'Metro has not created a user profile yet. Launch Metro once without the mod, close it, then run this installer again.'
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

function Set-MetroGraphicsSettings([string]$path, [object[]]$settings) {
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $document = Read-TextFile $path
        $text = $document.Text
        $encoding = $document.Encoding
    }
    else {
        $text = ''
        $encoding = New-Object Text.UTF8Encoding($false)
    }

    $newline = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $records = New-Object System.Collections.Generic.List[object]
    foreach ($setting in $settings) {
        $key = [string]$setting.Key
        $value = [string]$setting.Value
        $linePattern = "(?m)^[ \t]*$([regex]::Escape($key))[ \t]+([^\s\r\n]+)[ \t]*(?=\r?$)"
        $settingMatches = [regex]::Matches($text, $linePattern)
        if ($settingMatches.Count -gt 1) {
            throw "user.cfg contains more than one $key entry; no graphics setting was changed."
        }

        $existed = $settingMatches.Count -eq 1
        $previous = if ($existed) { $settingMatches[0].Groups[1].Value } else { $null }
        $records.Add([pscustomobject]@{
            key = $key
            displayName = [string]$setting.Name
            settingExistedBefore = $existed
            previousValue = $previous
            installedValue = $value
        })

        if ($existed) {
            $text = [regex]::Replace($text, $linePattern, "$key $value")
        }
        else {
            $prefix = if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) { $newline } else { '' }
            $text = $text + $prefix + "$key $value" + $newline
        }
    }

    [IO.File]::WriteAllText($path, $text, $encoding)
    return $records
}

try {
    if (Get-Process -Name metro -ErrorAction SilentlyContinue) {
        Stop-WithMessage 'Metro 2033 Redux is running. Close the game, then run the installer again.'
    }

    $detectedStore = 'Unknown'
    if (-not $GamePath) {
        $installation = Find-MetroInstallation
        if ($installation) {
            $GamePath = $installation.Path
            $detectedStore = $installation.Store
        }
    }
    if (-not $GamePath -and -not $NonInteractive) {
        $GamePath = Read-Host 'Metro was not found automatically. Enter the folder containing metro.exe'
    }
    if (-not $GamePath) {
        Stop-WithMessage 'Metro 2033 Redux could not be found.'
    }

    $GamePath = [IO.Path]::GetFullPath($GamePath.Trim('"'))
    if ($detectedStore -eq 'Unknown') {
        foreach ($candidate in @(Get-MetroGameCandidates -SteamRoot $SteamRoot -EpicManifestRoot $EpicManifestRoot)) {
            if ([IO.Path]::GetFullPath($candidate.Path).Equals($GamePath, [StringComparison]::OrdinalIgnoreCase)) {
                $detectedStore = $candidate.Store
                break
            }
        }
    }
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

    if (-not $UserConfigPath) {
        $UserConfigPath = Resolve-MetroUserConfig -store $detectedStore
    }
    $graphicsConfigPath = [IO.Path]::GetFullPath($UserConfigPath.Trim('"'))
    $graphicsConfigDirectory = Split-Path -Parent $graphicsConfigPath
    if (-not (Test-Path -LiteralPath $graphicsConfigDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $graphicsConfigDirectory -Force | Out-Null
    }

    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $backupRoot = Join-Path $GamePath "Metro2033ReduxVR_Backups\$stamp"
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null

    $graphicsConfigExisted = Test-Path -LiteralPath $graphicsConfigPath -PathType Leaf
    if ($graphicsConfigExisted) {
        $graphicsBackupPath = Join-Path $backupRoot 'user.cfg.pre-install'
        Copy-Item -LiteralPath $graphicsConfigPath -Destination $graphicsBackupPath -Force
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

    # Keep every tester on the renderer state used to validate the VR routes.
    # Metro labels r_supersample=1 as SSAA Off and r_af_level=1 as 16x texture
    # filtering; these values were confirmed from the game's persisted config.
    $graphicsConfigChanged = $true
    $graphicsSettingsState = @(Set-MetroGraphicsSettings -path $graphicsConfigPath -settings $requiredGraphicsSettings)
    $installedConfigText = (Read-TextFile $graphicsConfigPath).Text
    foreach ($setting in $requiredGraphicsSettings) {
        $key = [string]$setting.Key
        $expectedValue = [string]$setting.Value
        $pattern = "(?m)^[ \t]*$([regex]::Escape($key))[ \t]+([^\s\r\n]+)[ \t]*(?=\r?$)"
        $installedMatches = [regex]::Matches($installedConfigText, $pattern)
        if ($installedMatches.Count -ne 1 -or
            -not $installedMatches[0].Groups[1].Value.Equals($expectedValue, [StringComparison]::OrdinalIgnoreCase)) {
            throw "$($setting.Name) verification failed after updating user.cfg."
        }
    }

    $installRecord = [ordered]@{
        product = 'Metro2033ReduxVR'
        release = $payloadManifest.release
        sourceCommit = $payloadManifest.sourceCommit
        installedAt = (Get-Date).ToString('o')
        gamePath = $GamePath
        gameExeSha256 = $actualExeSha256
        backupDirectory = $backupRoot
        graphicsSettings = [ordered]@{
            configPath = $graphicsConfigPath
            configExistedBefore = $graphicsConfigExisted
            settings = @($graphicsSettingsState)
        }
        files = $installed
    }
    $recordPath = Join-Path $GamePath 'Metro2033ReduxVR.install.json'
    $installRecord | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $recordPath -Encoding UTF8

    Write-Host ''
    Write-Host 'Metro2033ReduxVR installed successfully.' -ForegroundColor Green
    Write-Host "Game folder: $GamePath"
    Write-Host "Backup:     $backupRoot"
    Write-Host "Metro config: $graphicsConfigPath"
    Write-Host 'Metro graphics: Medium quality, Very High tessellation, VSync Off, SSAA Off, 16x texture filtering'
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
    if ($graphicsConfigChanged -and $GamePath) {
        if ($graphicsConfigExisted -and $graphicsBackupPath -and
            (Test-Path -LiteralPath $graphicsBackupPath -PathType Leaf)) {
            Copy-Item -LiteralPath $graphicsBackupPath -Destination $graphicsConfigPath -Force
        }
        elseif (-not $graphicsConfigExisted -and
            (Test-Path -LiteralPath $graphicsConfigPath -PathType Leaf)) {
            Remove-Item -LiteralPath $graphicsConfigPath -Force
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
