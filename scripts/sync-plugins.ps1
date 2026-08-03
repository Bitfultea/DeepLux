# =============================================================================
# DeepLux Plugin Sync Script (Windows / PowerShell)
# =============================================================================
# Sync built plugin .dll files from build directory to ~/.deeplux/plugins/
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File sync-plugins.ps1 [BUILD_DIR]
# =============================================================================

param(
    [string]$BuildDir = (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
)

$ErrorActionPreference = "Stop"
$PluginsHome = Join-Path $env:USERPROFILE ".deeplux\plugins"

if (-not (Test-Path $PluginsHome)) {
    New-Item -ItemType Directory -Path $PluginsHome -Force | Out-Null
}

# Collect all plugin .dll files from build/lib
$soFiles = Get-ChildItem -Path (Join-Path $BuildDir "lib") -Filter "lib*Plugin.dll" -ErrorAction SilentlyContinue
if (-not $soFiles) {
    Write-Host "[WARN] No plugin .dll files found in $BuildDir\lib"
    exit 0
}

# Build a map: plugin name -> dll path
$soMap = @{}
foreach ($f in $soFiles) {
    $name = $f.BaseName -replace '^lib','' -replace 'Plugin$',''
    $soMap[$name] = $f.FullName
}

Write-Host "[INFO] Found $($soMap.Count) plugin .dll files in build directory"
Write-Host "[INFO] Target plugin home: $PluginsHome"
Write-Host ""

# Scan source metadata.json files
$srcRoot = Join-Path $BuildDir "..\src\plugins"
$metadataFiles = Get-ChildItem -Path $srcRoot -Filter "metadata.json" -Recurse -ErrorAction SilentlyContinue

$copied = 0
$skipped = 0
$prepared = 0

foreach ($metaFile in $metadataFiles) {
    $meta = Get-Content $metaFile.FullName -Raw | ConvertFrom-Json
    $metaName = $meta.name
    if (-not $metaName) { continue }

    $dirName = $metaFile.Directory.Name
    $shortDir = $dirName -replace 'Plugin$',''

    # Find matching .dll
    $matchName = $null
    foreach ($candidate in @($metaName, $dirName, $shortDir)) {
        if ($soMap.ContainsKey($candidate)) {
            $matchName = $candidate
            break
        }
    }
    if (-not $matchName) { continue }

    $targetDir = Join-Path $PluginsHome $dirName
    if (-not (Test-Path $targetDir)) {
        New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    }

    # Copy metadata
    $targetMeta = Join-Path $targetDir "metadata.json"
    if (-not (Test-Path $targetMeta) -or ((Get-Content $metaFile.FullName -Raw) -ne (Get-Content $targetMeta -Raw))) {
        Copy-Item $metaFile.FullName $targetMeta -Force
        $prepared++
    }

    # Copy .dll
    $srcDll = $soMap[$matchName]
    $dstDll = Join-Path $targetDir (Split-Path -Leaf $srcDll)
    if (-not (Test-Path $dstDll) -or (Test-Path $srcDll)) {
        Copy-Item $srcDll $dstDll -Force
        Write-Host "[COPY]  $metaName`: $(Split-Path -Leaf $srcDll)"
        $copied++
    } else {
        $skipped++
    }
}

Write-Host ""
Write-Host "[DONE] prepared=$prepared copied=$copied skipped=$skipped"
