# =============================================================================
# DeepLux Windows deployment script
# =============================================================================
# Produces a relocatable Windows installation tree:
#
#   <Prefix>\bin\DeepLux.exe          (DeepLux + Qt + OpenCV runtime)
#   <Prefix>\plugins\<PluginName>\    (bundled runtime plugins + metadata.json)
#   <Prefix>\tools\sam_server\        (optional Python SAM service)
#   <Prefix>\share\deeplux\           (plugin manifest etc.)
#
# Usage (from a "Developer PowerShell for VS" or any shell with cmake in PATH):
#   powershell -ExecutionPolicy Bypass -File scripts\deploy-windows.ps1 -BuildDir build-win
#   powershell ... -BuildDir build-win -Prefix D:\dist\DeepLux -Zip
#
# The build directory is the one produced by:
#   cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release ^
#         -DDEEPLUX_QT_PATH=C:/Qt/6.8.3/msvc2022_64 ^
#         -DOpenCV_DIR=C:/opencv/build -DBUILD_TESTS=OFF -DDEEPLUX_NATIVE_SAM=OFF
#   cmake --build build-win
# =============================================================================

param(
    [Parameter(Position = 0)]
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "build-win"),
    [Parameter(Position = 1)]
    [string]$Prefix = (Join-Path (Split-Path -Parent $PSScriptRoot) "dist\DeepLux-win64"),
    [string]$QtBin = "",
    [string]$OpenCvBin = "",
    [string]$CMake = "",
    [string]$Config = "",
    [switch]$SkipSmokeTest,
    [switch]$Zip
)

$ErrorActionPreference = "Stop"

function Get-CacheValue([string]$Name) {
    $cache = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path $cache)) { return $null }
    $match = Select-String -Path $cache -Pattern "^$([regex]::Escape($Name)):[^=]*=(.*)$" |
        Select-Object -First 1
    if ($match) { return $match.Matches[0].Groups[1].Value.Trim() }
    return $null
}

# Locate the Visual C++ installation so windeployqt can deploy the compiler
# runtime even when the script is started from a plain PowerShell window.
function Resolve-VcInstallDir {
    $compiler = Get-CacheValue "CMAKE_CXX_COMPILER"
    if ($compiler) {
        $normalized = $compiler.Replace('/', '\')
        $marker = '\VC\Tools\MSVC\'
        $index = $normalized.IndexOf($marker, [System.StringComparison]::OrdinalIgnoreCase)
        if ($index -gt 0) {
            return (Join-Path $normalized.Substring(0, $index) "VC")
        }
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -property installationPath 2>$null
        if ($vsPath) { return (Join-Path $vsPath "VC") }
    }
    return $null
}

if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    Write-Host "[ERROR] CMake build directory not found: $BuildDir"
    exit 1
}
# Multi-config generators (Visual Studio, Ninja Multi-Config) put the runtime
# output into bin/<Config>. Resolve the actual runtime directory instead of
# assuming a single-config Ninja layout.
$isMultiConfig = [bool](Get-CacheValue "CMAKE_CONFIGURATION_TYPES")
if (-not $Config -and $isMultiConfig) { $Config = "Release" }
$runtimeCandidates = @()
if ($Config) { $runtimeCandidates += (Join-Path $BuildDir (Join-Path "bin" $Config)) }
$runtimeCandidates += (Join-Path $BuildDir "bin")
$runtimeDir = $null
foreach ($candidate in $runtimeCandidates) {
    if (Test-Path (Join-Path $candidate "DeepLux.exe")) {
        $runtimeDir = $candidate
        break
    }
}
if (-not $runtimeDir) {
    Write-Host "[ERROR] DeepLux.exe not found under: $($runtimeCandidates -join ', '); build the project first."
    exit 1
}

# --- Resolve Qt ---------------------------------------------------------------
if (-not $QtBin) {
    $qtRoot = Get-CacheValue "DEEPLUX_QT_PATH"
    if (-not $qtRoot) {
        $qt6Dir = Get-CacheValue "Qt6_DIR"     # <qt>/lib/cmake/Qt6
        if ($qt6Dir) {
            $qtRoot = [System.IO.Path]::GetFullPath((Join-Path $qt6Dir "..\..\.."))
        }
    }
    if ($qtRoot) { $QtBin = Join-Path $qtRoot "bin" }
}
if (-not $QtBin -or -not (Test-Path (Join-Path $QtBin "windeployqt.exe"))) {
    Write-Host "[ERROR] windeployqt.exe not found. Pass -QtBin <path-to-Qt\bin>."
    exit 1
}
$windeployqt = Join-Path $QtBin "windeployqt.exe"

# --- Resolve OpenCV -----------------------------------------------------------
# Official Windows packs split runtime files between <opencv>/build/bin and
# <opencv>/build/x64/vc16/bin (opencv_world*.dll only exists in the latter),
# while vcpkg keeps them in installed/<triplet>/bin. Collect from every
# candidate and de-duplicate by file name instead of taking the first hit.
$opencvDlls = @()
$opencvLabel = "(not found)"
if ($OpenCvBin) {
    if (Test-Path $OpenCvBin) {
        $opencvDlls = @(Get-ChildItem -Path $OpenCvBin -Filter "opencv*.dll" -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notmatch 'd\.dll$' })
        $opencvLabel = (Resolve-Path $OpenCvBin).Path
    }
} else {
    $opencvDir = Get-CacheValue "OpenCV_DIR"
    if ($opencvDir) {
        $candidates = @(
            (Join-Path $opencvDir "bin"),
            (Join-Path $opencvDir "x64\vc16\bin"),
            (Join-Path $opencvDir "..\bin"),
            (Join-Path $opencvDir "..\..\bin")
        )
        $seen = @{}
        foreach ($candidate in $candidates) {
            if (-not (Test-Path $candidate)) { continue }
            Get-ChildItem -Path $candidate -Filter "opencv*.dll" -File -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -notmatch 'd\.dll$' } |
                ForEach-Object { $seen[$_.Name] = $_.FullName }
        }
        if ($seen.Count -gt 0) {
            $opencvDlls = @($seen.Values | ForEach-Object { Get-Item $_ })
            $opencvLabel = "auto ($($opencvDlls.Count) DLLs)"
        }
    }
}
if ($opencvDlls.Count -eq 0) {
    Write-Host "[WARN] OpenCV runtime DLLs not found; the installed app will not start without opencv DLLs."
}

# --- Locate cmake -------------------------------------------------------------
# Prefer PATH (Developer PowerShell), then the CMake path recorded in the build
# cache so the script also works from a plain PowerShell window.
if (-not $CMake) {
    $cmakeFromPath = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if ($cmakeFromPath) {
        $CMake = $cmakeFromPath
    } else {
        $cmakeFromCache = Get-CacheValue "CMAKE_COMMAND"
        if ($cmakeFromCache -and (Test-Path $cmakeFromCache)) {
            $CMake = $cmakeFromCache
        }
    }
}
if (-not $CMake) {
    Write-Host "[ERROR] cmake not found (PATH and CMakeCache.txt). Run this script from a Visual Studio Developer PowerShell."
    exit 1
}

# --- Prepare MSVC/SDK environment for windeployqt ----------------------------
if (-not $env:VCINSTALLDIR) {
    $vcDir = Resolve-VcInstallDir
    if ($vcDir -and (Test-Path $vcDir)) {
        $env:VCINSTALLDIR = $vcDir
    }
}
if (-not $env:VCINSTALLDIR) {
    Write-Host "[WARN] VCINSTALLDIR not found; windeployqt may skip the VC++ runtime. Run from Developer PowerShell for a complete deployment."
}

$sdkBin = $null
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
if (Test-Path $sdkRoot) {
    $sdkVersionDir = Get-ChildItem $sdkRoot -Directory |
        Where-Object { $_.Name -match '^\d+\.' } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($sdkVersionDir) {
        $candidate = Join-Path $sdkVersionDir.FullName "x64"
        if (Test-Path (Join-Path $candidate "dxcompiler.dll")) {
            $sdkBin = $candidate
            $env:PATH = "$sdkBin;$env:PATH"
        }
    }
}

Write-Host "[INFO] Build dir : $BuildDir"
Write-Host "[INFO] Config    : $(if ($Config) { $Config } else { '(single-config)' })"
Write-Host "[INFO] Runtime   : $runtimeDir"
Write-Host "[INFO] Prefix    : $Prefix"
Write-Host "[INFO] Qt bin    : $QtBin"
Write-Host "[INFO] OpenCV    : $opencvLabel"
Write-Host ""

# --- Install app, plugins and data --------------------------------------------
if (Test-Path $Prefix) { Remove-Item -Recurse -Force $Prefix }
$installArgs = @("--install", $BuildDir, "--prefix", $Prefix)
if ($isMultiConfig) { $installArgs += @("--config", $Config) }
& $CMake @installArgs
if ($LASTEXITCODE -ne 0) { throw "cmake --install failed" }

# --- Deploy Qt runtime --------------------------------------------------------
$binDir = Join-Path $Prefix "bin"
& $windeployqt --release --no-translations --compiler-runtime --dir $binDir (Join-Path $binDir "DeepLux.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

# windeployqt can miss these when VCINSTALLDIR / Windows SDK are not on PATH;
# copy them explicitly so the deployed tree also works on a clean machine.
if (-not (Test-Path (Join-Path $binDir "vc_redist.x64.exe")) -and $env:VCINSTALLDIR) {
    $redistRoot = Join-Path $env:VCINSTALLDIR "Redist\MSVC"
    $redist = Get-ChildItem -Path $redistRoot -Recurse -Filter "vc_redist.x64.exe" -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($redist) {
        Copy-Item $redist.FullName (Join-Path $binDir "vc_redist.x64.exe") -Force
    }
}
if ($sdkBin) {
    foreach ($name in @("dxcompiler.dll", "dxil.dll")) {
        $source = Join-Path $sdkBin $name
        if ((Test-Path $source) -and -not (Test-Path (Join-Path $binDir $name))) {
            Copy-Item $source (Join-Path $binDir $name) -Force
        }
    }
}

# --- Deploy OpenCV runtime ----------------------------------------------------
foreach ($dll in $opencvDlls) {
    Copy-Item $dll.FullName (Join-Path $binDir $dll.Name) -Force
}

# --- Report -------------------------------------------------------------------
$pluginCount = @(Get-ChildItem -Path (Join-Path $Prefix "plugins") -Filter "*.dll" -Recurse -File -ErrorAction SilentlyContinue).Count
Write-Host ""
Write-Host "[DONE] Installed to $Prefix"
Write-Host "       plugins: $pluginCount"
Write-Host "       run    : $(Join-Path $binDir 'DeepLux.exe') --gui"

# --- Smoke test: isolated user dir + independent manifest ---------------------
if (-not $SkipSmokeTest) {
    $savedPath = $env:PATH
    $savedAppData = $env:DEEPLUX_APP_DATA_DIR
    $savedUserProfile = $env:USERPROFILE
    $savedHome = $env:HOME
    $stdoutFile = Join-Path $env:TEMP ("deeplux-smoke-" + [guid]::NewGuid().ToString("N") + ".txt")
    $stderrFile = Join-Path $env:TEMP ("deeplux-smoke-" + [guid]::NewGuid().ToString("N") + ".err")
    $isolatedData = Join-Path $env:TEMP ("deeplux-smoke-data-" + [guid]::NewGuid().ToString("N"))
    try {
        New-Item -ItemType Directory -Path $isolatedData -Force | Out-Null
        # Isolate the user plugin dir so a stray ~/.deeplux cannot mask a missing
        # packaged plugin. DeepLux resolves pluginPath() from DEEPLUX_APP_DATA_DIR.
        $env:DEEPLUX_APP_DATA_DIR = $isolatedData
        $env:USERPROFILE = $isolatedData
        $env:HOME = $isolatedData
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"

        # Independent expected set: the build-generated manifest, NOT the installed
        # tree (otherwise deleting a plugin dir silently shrinks the expectation).
        $manifestPath = Join-Path $BuildDir "plugins.manifest"
        if (-not (Test-Path $manifestPath)) {
            $manifestPath = Join-Path $Prefix "share\deeplux\plugins.manifest"
        }
        if (-not (Test-Path $manifestPath)) {
            throw "deployed CLI smoke test failed: plugins.manifest not found (build or installed tree)"
        }
        $manifestDirs = @(Get-Content $manifestPath -Encoding UTF8 |
            ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
        if ($manifestDirs.Count -eq 0) {
            throw "deployed CLI smoke test failed: plugins.manifest is empty ($manifestPath)"
        }

        # Cross-check against plugin-facts.json (single source of truth).
        $factsPath = Join-Path $BuildDir "plugin-facts.json"
        if (-not (Test-Path $factsPath)) {
            $factsPath = Join-Path $Prefix "share\deeplux\plugin-facts.json"
        }
        if (-not (Test-Path $factsPath)) {
            throw "deployed CLI smoke test failed: plugin-facts.json not found"
        }
        $facts = Get-Content $factsPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([int]$facts.packaged_plugin_count -ne $manifestDirs.Count) {
            throw ("deployed CLI smoke test failed: facts packaged_plugin_count={0} != manifest count={1}" -f `
                $facts.packaged_plugin_count, $manifestDirs.Count)
        }

        # Every manifest entry must have its directory, metadata and a library.
        $pluginRoot = Join-Path $Prefix "plugins"
        $expectedModules = @()
        $missingDir = @()
        $missingMeta = @()
        $missingLib = @()
        foreach ($dir in $manifestDirs) {
            $pdir = Join-Path $pluginRoot $dir
            if (-not (Test-Path $pdir)) { $missingDir += $dir; continue }
            $metaPath = Join-Path $pdir "metadata.json"
            if (-not (Test-Path $metaPath)) { $missingMeta += $dir; continue }
            $dlls = @(Get-ChildItem -Path $pdir -Filter "*.dll" -File -ErrorAction SilentlyContinue)
            if ($dlls.Count -eq 0) { $missingLib += $dir; continue }
            try { $j = Get-Content $metaPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $missingMeta += $dir; continue }
            $ifaces = @($j.interfaces)
            $isCamera = ($j.category -eq 'camera') -or ($ifaces -contains 'ICameraPlugin')
            if (-not $isCamera -and $j.name) { $expectedModules += [string]$j.name }
        }
        $expectedModules = @($expectedModules | Sort-Object -Unique)

        $exe = Join-Path $binDir "DeepLux.exe"
        # DeepLux is a WIN32 (GUI subsystem) executable: use Start-Process with
        # redirected streams instead of `&`, which does not reliably wait for or
        # capture GUI-subsystem processes.
        $process = Start-Process -FilePath $exe -ArgumentList "--cli", "list-plugins" -Wait -PassThru `
            -NoNewWindow -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile
        $output = Get-Content $stdoutFile -Encoding UTF8 -ErrorAction SilentlyContinue

        $loaded = 0
        $okNames = @()
        $failNames = @()
        $match = $output | Select-String -Pattern "Loaded Plugins \((\d+)\)" | Select-Object -First 1
        if ($match -and $match.Matches.Count -gt 0) {
            $loaded = [int]$match.Matches[0].Groups[1].Value
        }
        foreach ($line in $output) {
            if ($line -match '^\[(OK|FAIL)\]\s+(.+?)\s+v[0-9]') {
                if ($Matches[1] -eq 'OK') { $okNames += $Matches[2] } else { $failNames += $Matches[2] }
            }
        }
        $missingModules = @($expectedModules | Where-Object { $okNames -notcontains $_ })

        Write-Host ("[SMOKE] exit={0} loaded={1} expectedModules={2} manifest={3} ok={4} fail={5} missingDir={6} missingMeta={7} missingLib={8}" -f `
            $process.ExitCode, $loaded, $expectedModules.Count, $manifestDirs.Count, $okNames.Count, $failNames.Count, `
            $missingDir.Count, $missingMeta.Count, $missingLib.Count)

        $problems = @()
        if ($missingDir.Count -gt 0) { $problems += "missing plugin dir(s): " + ($missingDir -join ', ') }
        if ($missingMeta.Count -gt 0) { $problems += "missing/invalid metadata: " + ($missingMeta -join ', ') }
        if ($missingLib.Count -gt 0) { $problems += "missing library (.dll): " + ($missingLib -join ', ') }
        if ($failNames.Count -gt 0) { $problems += "plugin(s) failed to load: " + ($failNames -join ', ') }
        if ($missingModules.Count -gt 0) { $problems += "module(s) not loaded: " + ($missingModules -join ', ') }
        if ($problems.Count -gt 0) {
            Write-Host ($output | Select-Object -Last 10 | Out-String)
            Write-Host (Get-Content $stderrFile -Encoding UTF8 -ErrorAction SilentlyContinue | Out-String)
            throw ("deployed CLI smoke test failed: " + ($problems -join '; '))
        }
        if ($process.ExitCode -ne 0 -or $loaded -ne $expectedModules.Count) {
            Write-Host ($output | Select-Object -Last 10 | Out-String)
            throw "deployed CLI smoke test failed: expected $($expectedModules.Count) modules, got $loaded (exit=$($process.ExitCode))"
        }
    } finally {
        $env:PATH = $savedPath
        $env:DEEPLUX_APP_DATA_DIR = $savedAppData
        $env:USERPROFILE = $savedUserProfile
        $env:HOME = $savedHome
        Remove-Item $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
        Remove-Item $isolatedData -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# --- Optional zip -------------------------------------------------------------
if ($Zip) {
    $zipPath = "$Prefix.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path (Join-Path $Prefix "*") -DestinationPath $zipPath
    Write-Host "[ZIP] $zipPath"
}
