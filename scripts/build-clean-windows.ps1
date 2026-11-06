<#
.SYNOPSIS
清理 Windows 构建目录中的全部对象文件后全量重建，可选运行全量 CTest。

.DESCRIPTION
本机 CMake 的中文 showIncludes 前缀会导致头文件依赖追踪失效：改动 .h 后旧
.obj 不会重编，可能出现“改了没重编”的假通过，或旧签名导致链接错误。
执行 git merge / pull / 修改头文件后，建议先跑本脚本。

.PARAMETER BuildDir
构建目录（相对仓库根），默认 build-win-tests。

.PARAMETER Jobs
并行度，默认 8。

.PARAMETER RunTests
构建成功后运行 ctest --output-on-failure。

.PARAMETER VcVars
vcvars64.bat 路径。

.PARAMETER CMakeExe
cmake.exe 路径（ctest.exe 从同目录推导）。

.PARAMETER QtBin
Qt bin 目录（运行测试时需要）。

.PARAMETER OpenCvBin
OpenCV bin 目录（运行测试时需要）。

.EXAMPLE
pwsh -File scripts/build-clean-windows.ps1

.EXAMPLE
pwsh -File scripts/build-clean-windows.ps1 -RunTests
#>
param(
    [string]$BuildDir = "build-win-tests",
    [int]$Jobs = 8,
    [switch]$RunTests,
    [string]$VcVars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat",
    [string]$CMakeExe = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    [string]$QtBin = "D:\deps\Qt\bin",
    [string]$OpenCvBin = "D:\deps\opencv\build\x64\vc16\bin"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $repo $BuildDir
$ctestExe = Join-Path (Split-Path $CMakeExe -Parent) "ctest.exe"

foreach ($p in @($VcVars, $CMakeExe, $buildPath)) {
    if (-not (Test-Path -LiteralPath $p)) {
        throw "找不到依赖：$p（可用参数覆盖；构建目录需先 cmake configure）"
    }
}

Push-Location $repo
try {
    $objs = @(Get-ChildItem -Path (Join-Path $buildPath "*") -Recurse -Include *.obj -File -ErrorAction SilentlyContinue)
    Write-Host "清理 $($objs.Count) 个对象文件：$BuildDir"
    if ($objs.Count -gt 0) {
        $objs | Remove-Item -Force
    }

    $buildCmd = 'call "{0}" >nul 2>nul && "{1}" --build "{2}" -j {3}' -f $VcVars, $CMakeExe, $BuildDir, $Jobs
    Write-Host "全量重建：$BuildDir -j $Jobs"
    cmd /c $buildCmd
    if ($LASTEXITCODE -ne 0) {
        throw "构建失败，exit=$LASTEXITCODE"
    }

    if ($RunTests) {
        $env:PATH = "$QtBin;$OpenCvBin;$env:PATH"
        Write-Host "运行全量测试：$BuildDir"
        & $ctestExe --test-dir $buildPath --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            throw "测试失败，exit=$LASTEXITCODE"
        }
    }
}
finally {
    Pop-Location
}

Write-Host "完成"
