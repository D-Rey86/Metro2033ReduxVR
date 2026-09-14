[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeDirectory,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [string]$ReleaseName = 'Metro2033ReduxVR-Test',
    [string]$SourceCommit
)

$ErrorActionPreference = 'Stop'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = Split-Path -Parent $scriptRoot
$templateRoot = Join-Path $scriptRoot 'Tester'
$runtimeRoot = [IO.Path]::GetFullPath($RuntimeDirectory)
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)

if (-not (Test-Path -LiteralPath $runtimeRoot -PathType Container)) {
    throw "Runtime directory does not exist: $runtimeRoot"
}
if (Test-Path -LiteralPath $outputRoot) {
    throw "Output already exists; choose a new directory: $outputRoot"
}
if (-not (Test-Path -LiteralPath $templateRoot -PathType Container)) {
    throw "Installer template is missing: $templateRoot"
}

function Get-IniSettingValues([string]$text, [string]$section, [string]$key) {
    $currentSection = ''
    $values = New-Object System.Collections.Generic.List[string]
    $escapedKey = [regex]::Escape($key)
    foreach ($line in [regex]::Split($text, '\r?\n')) {
        if ($line -match '^\s*\[([^\]]+)\]\s*(?:;.*)?$') {
            $currentSection = $matches[1].Trim()
            continue
        }
        if ($currentSection -ieq $section -and
            $line -match "^\s*$escapedKey\s*=\s*([^;]*?)\s*(?:;.*)?$") {
            $values.Add($matches[1].Trim())
        }
    }
    return @($values)
}

function Assert-ReleaseConfiguration([string]$path) {
    $text = Get-Content -LiteralPath $path -Raw
    $requirements = @(
        @{ Section = 'Logging'; Key = 'calls'; Value = '0' },
        @{ Section = 'Logging'; Key = 'input'; Value = '0' },
        @{ Section = 'Logging'; Key = 'debug'; Value = '0' },
        @{ Section = 'Logging'; Key = 'unbuffered'; Value = '0' },
        @{ Section = 'Logging'; Key = 'convergence'; Value = '0' },
        @{ Section = 'Logging'; Key = 'separation'; Value = '0' },
        @{ Section = 'Hunting'; Key = 'hunting'; Value = '0' },
        @{ Section = 'Rendering'; Key = 'export_fixed'; Value = '0' },
        @{ Section = 'Rendering'; Key = 'export_shaders'; Value = '0' },
        @{ Section = 'Rendering'; Key = 'export_hlsl'; Value = '0' },
        @{ Section = 'Rendering'; Key = 'dump_usage'; Value = '0' }
    )
    foreach ($requirement in $requirements) {
        $values = @(Get-IniSettingValues -text $text -section $requirement.Section -key $requirement.Key)
        if ($values.Count -ne 1 -or $values[0] -ne $requirement.Value) {
            throw "Release package rejected: [$($requirement.Section)] $($requirement.Key) must appear exactly once with value $($requirement.Value) in d3dx.ini."
        }
    }
}

$requiredFiles = @(
    'd3d11.dll',
    'd3dx.ini',
    'openvr_api.dll',
    'nvapi64.dll',
    'D3DCompiler_47.dll',
    'ShaderFixes\2b8e982a84191f98-ps_replace.bin',
    'ShaderFixes\8bec50a445270a9d-ps_replace.bin',
    'ShaderFixes\cc6b75435b2c136e-ps_replace.bin',
    'ShaderFixes\f0485ad68b8f0ae3-ps_replace.bin',
    'ShaderFixes\f40a4f7a8d939cb8-ps_replace.bin',
    'ShaderFixes\fbb33c62c13b8556-ps_replace.bin'
)

foreach ($relativePath in $requiredFiles) {
    $sourcePath = Join-Path $runtimeRoot $relativePath
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "Required runtime file is missing: $relativePath"
    }
}

# A tester package must never inherit 3Dmigoto's development defaults. Besides
# drawing the legacy green stereo overlay, API logging and shader hunting select
# substantially heavier runtime paths. Reject the package before creating any
# output if the supplied runtime configuration is not release-safe.
Assert-ReleaseConfiguration -path (Join-Path $runtimeRoot 'd3dx.ini')

New-Item -ItemType Directory -Path $outputRoot | Out-Null
Copy-Item -Path (Join-Path $templateRoot '*') -Destination $outputRoot -Recurse
$payloadRoot = Join-Path $outputRoot 'Payload'
New-Item -ItemType Directory -Path $payloadRoot | Out-Null

$manifestFiles = foreach ($relativePath in $requiredFiles) {
    $sourcePath = Join-Path $runtimeRoot $relativePath
    $destinationPath = Join-Path $payloadRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $destinationPath) -Force | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination $destinationPath
    $item = Get-Item -LiteralPath $destinationPath
    [ordered]@{
        path = $relativePath
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $destinationPath -Algorithm SHA256).Hash.ToUpperInvariant()
    }
}

if (-not $SourceCommit) {
    $SourceCommit = (& git -C $repositoryRoot rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) { $SourceCommit = 'uncommitted-source' }
}

$manifest = [ordered]@{
    product = 'Metro2033ReduxVR'
    release = $ReleaseName
    sourceCommit = $SourceCommit
    supportedGame = [ordered]@{
        edition = 'Metro 2033 Redux Windows x64'
        fileVersion = '1.0.0.3'
        metroExeSha256 = '183EF65212E351C55A2832C3F3C8B04616B153697913D33B1E27683163F14E15'
    }
    files = @($manifestFiles)
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputRoot 'payload-manifest.json') -Encoding UTF8

$licenseRoot = Join-Path $outputRoot 'Licenses'
New-Item -ItemType Directory -Path $licenseRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') -Destination (Join-Path $licenseRoot '3Dmigoto-GPL-3.0.txt')
Copy-Item -LiteralPath (Join-Path $repositoryRoot 'ThirdParty\openvr\LICENSE') -Destination (Join-Path $licenseRoot 'OpenVR-LICENSE.txt')

Write-Host "Created verified package directory: $outputRoot"
Write-Host 'Inspect and test this directory before creating a public archive.'
