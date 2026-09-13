function Get-DefaultSteamRoot {
    return (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
}

function Get-DefaultEpicManifestRoot {
    if (-not $env:ProgramData) { return $null }
    return Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
}

function Get-MetroGameCandidates {
    [CmdletBinding()]
    param(
        [AllowNull()][string]$SteamRoot = (Get-DefaultSteamRoot),
        [AllowNull()][string]$EpicManifestRoot = (Get-DefaultEpicManifestRoot)
    )

    $results = New-Object System.Collections.Generic.List[object]
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)

    function Add-Candidate([string]$path, [string]$store) {
        if (-not $path) { return }
        try { $fullPath = [IO.Path]::GetFullPath($path.Trim('"')) }
        catch { return }
        if (-not (Test-Path -LiteralPath (Join-Path $fullPath 'metro.exe') -PathType Leaf)) { return }
        if ($seen.Add($fullPath)) {
            $results.Add([pscustomobject]@{ Path = $fullPath; Store = $store })
        }
    }

    if ($SteamRoot) {
        Add-Candidate (Join-Path $SteamRoot 'steamapps\common\Metro 2033 Redux') 'Steam'
        $libraryFile = Join-Path $SteamRoot 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $libraryFile -PathType Leaf) {
            foreach ($line in Get-Content -LiteralPath $libraryFile) {
                if ($line -match '^\s*"path"\s+"([^"]+)"') {
                    $library = $Matches[1] -replace '\\\\', '\'
                    Add-Candidate (Join-Path $library 'steamapps\common\Metro 2033 Redux') 'Steam'
                }
            }
        }
    }

    if ($EpicManifestRoot -and (Test-Path -LiteralPath $EpicManifestRoot -PathType Container)) {
        foreach ($manifestPath in Get-ChildItem -LiteralPath $EpicManifestRoot -Filter '*.item' -File -ErrorAction SilentlyContinue) {
            try { $manifest = Get-Content -LiteralPath $manifestPath.FullName -Raw | ConvertFrom-Json }
            catch { continue }
            $displayName = [string]$manifest.DisplayName
            $launchExecutable = [string]$manifest.LaunchExecutable
            $isMetroName = $displayName -match '^Metro\s+2033\s+Redux$'
            $isMetroExecutable = $launchExecutable -and
                [IO.Path]::GetFileName(($launchExecutable -replace '/', '\')) -ieq 'metro.exe'
            if ($isMetroName -and $isMetroExecutable) {
                Add-Candidate ([string]$manifest.InstallLocation) 'Epic Games Store'
            }
        }
    }

    return $results.ToArray()
}
