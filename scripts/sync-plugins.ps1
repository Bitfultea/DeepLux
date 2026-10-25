# =============================================================================
# DeepLux Plugin Sync Script (Windows / PowerShell)
# =============================================================================
# Copy built plugin .dll files and their metadata.json into ~/.deeplux/plugins/,
# which is the user plugin directory scanned by DeepLux on every platform
# (PathUtils::pluginPath()).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File sync-plugins.ps1 [BUILD_DIR] [SOURCE_DIR]
#
# CMake invokes this as:
#   powershell -ExecutionPolicy Bypass -File sync-plugins.ps1 <build> <source>
# =============================================================================

param(
    [Parameter(Position = 0)]
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "build"),
    [Parameter(Position = 1)]
    [string]$SourceDir = (Split-Path -Parent $PSScriptRoot),
    [string]$PluginsHome = (Join-Path $env:USERPROFILE ".deeplux\plugins")
)

$ErrorActionPreference = "Stop"

$srcPlugins = Join-Path $SourceDir "src\plugins"
if (-not (Test-Path $srcPlugins)) {
    Write-Host "[ERROR] Plugin source directory not found: $srcPlugins"
    exit 1
}

if (-not (Test-Path $PluginsHome)) {
    New-Item -ItemType Directory -Path $PluginsHome -Force | Out-Null
}

# On Windows add_library(SHARED) puts plugin DLLs into the RUNTIME output dir
# (CMAKE_RUNTIME_OUTPUT_DIRECTORY = <build>/bin), not <build>/lib. Visual Studio
# and Ninja Multi-Config generators additionally append a per-config subdirectory
# (bin/Release), so search recursively. Accept the lib layout as well.
$searchDirs = @((Join-Path $BuildDir "bin"), (Join-Path $BuildDir "lib")) |
    Where-Object { Test-Path $_ }
$pluginLibs = @()
foreach ($dir in $searchDirs) {
    $pluginLibs += @(Get-ChildItem -Path $dir -Filter "*Plugin.dll" -File -Recurse -ErrorAction SilentlyContinue)
}

if ($pluginLibs.Count -eq 0) {
    Write-Host "[ERROR] No plugin *Plugin.dll files found under $BuildDir\bin or $BuildDir\lib"
    Write-Host "        Build the project first: cmake --build $BuildDir"
    exit 1
}

# Build a map: normalized plugin name -> dll path
$dllMap = @{}
foreach ($lib in $pluginLibs) {
    $name = $lib.BaseName -replace '^lib', '' -replace 'Plugin$', ''
    $dllMap[$name] = $lib.FullName
}

Write-Host "[INFO] Found $($dllMap.Count) plugin DLLs in build directory"
Write-Host "[INFO] Target plugin home: $PluginsHome"
Write-Host ""

$metadataFiles = @(Get-ChildItem -Path $srcPlugins -Filter "metadata.json" -Recurse -File -ErrorAction SilentlyContinue)
$copied = 0
$skipped = 0
$prepared = 0
$matched = @{}

foreach ($metaFile in $metadataFiles) {
    # metadata.json is UTF-8 without BOM. Windows PowerShell 5.1 decodes
    # Get-Content as ANSI by default, which corrupts non-ASCII labels and can
    # produce invalid JSON, so always specify UTF-8 explicitly.
    $metaRaw = Get-Content $metaFile.FullName -Raw -Encoding UTF8
    $metaName = $null
    try {
        $metaName = ($metaRaw | ConvertFrom-Json).name
    } catch {
        Write-Host "[WARN] Cannot parse $($metaFile.FullName) with ConvertFrom-Json; matching by directory name."
    }

    $dirName = $metaFile.Directory.Name
    $shortDir = $dirName -replace 'Plugin$', ''

    $matchName = $null
    foreach ($candidate in @($metaName, $dirName, $shortDir)) {
        if ($candidate -and $dllMap.ContainsKey($candidate)) {
            $matchName = $candidate
            break
        }
    }
    if (-not $matchName) { continue }
    $matched[$matchName] = $true

    $targetDir = Join-Path $PluginsHome $dirName
    if (-not (Test-Path $targetDir)) {
        New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    }

    # Copy metadata.json
    $targetMeta = Join-Path $targetDir "metadata.json"
    $metaChanged = $true
    if (Test-Path $targetMeta) {
        $metaChanged = $metaRaw -ne (Get-Content $targetMeta -Raw -Encoding UTF8)
    }
    if ($metaChanged) {
        Copy-Item $metaFile.FullName $targetMeta -Force
        $prepared++
    }

    # Copy DLL (compare hash: timestamps are unreliable across generators)
    $srcDll = $dllMap[$matchName]
    $dstDll = Join-Path $targetDir (Split-Path -Leaf $srcDll)
    $needsCopy = -not (Test-Path $dstDll)
    if (-not $needsCopy) {
        $srcHash = (Get-FileHash $srcDll -Algorithm SHA256).Hash
        $dstHash = (Get-FileHash $dstDll -Algorithm SHA256).Hash
        $needsCopy = $srcHash -ne $dstHash
    }
    if ($needsCopy) {
        Copy-Item $srcDll $dstDll -Force
        Write-Host "[COPY]  $metaName`: $(Split-Path -Leaf $srcDll)"
        $copied++
    } else {
        $skipped++
    }

    # Remove stale libraries of the same plugin (e.g. old `libFooPlugin.dll`
    # layouts) so QPluginLoader cannot pick up an ABI-mismatched binary.
    Get-ChildItem -Path $targetDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "*Plugin.dll" -and $_.FullName -ne $dstDll } |
        ForEach-Object {
            Write-Host "[DEL]   stale $($_.Name)"
            Remove-Item $_.FullName -Force
        }
}

# Plugins built without a matching metadata.json are not loadable; report them.
$unmatched = @($dllMap.Keys | Where-Object { -not $matched.ContainsKey($_) })
if ($unmatched.Count -gt 0) {
    Write-Host "[WARN] Built DLLs without matching metadata.json: $($unmatched -join ', ')"
}

Write-Host ""
Write-Host "[DONE] prepared=$prepared copied=$copied skipped=$skipped"
