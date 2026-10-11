# Builds daphne_core.dll for MIKU's Daphne extension (src/Daphne.Extension, MikuDeploy) and copies it with the
# FFmpeg DLLs to -Out. Fetches FFmpeg and the ASIO SDK on the first build.
# Needs CMake and Visual Studio (C++ desktop workload) on the build machine.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $root "third_party\ffmpeg\include\libavcodec\avcodec.h"))) { & (Join-Path $PSScriptRoot "fetch_ffmpeg.ps1") }
if (-not (Test-Path (Join-Path $root "third_party\asiosdk\common\iasiodrv.h"))) { & (Join-Path $PSScriptRoot "fetch_asiosdk.ps1") }
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath 2>$null
    if ($vs) { $c = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"; if (Test-Path $c) { $cmake = $c } }
}
if (-not $cmake) { throw "daphne: CMake not found (install Visual Studio with the C++ desktop workload, or CMake)" }
$bd = Join-Path $root "build-miku"
& $cmake -S $root -B $bd -DDAPHNE_BUILD_TESTS=OFF | Out-Host
if ($LASTEXITCODE -ne 0) { throw "daphne: CMake configure failed" }
& $cmake --build $bd --config $Config --target daphne_core | Out-Host
if ($LASTEXITCODE -ne 0) { throw "daphne: build failed" }
New-Item -ItemType Directory -Force $Out | Out-Null
$bin = Join-Path $bd "bin\$Config"
Copy-Item (Join-Path $bin "daphne_core.dll") $Out -Force
Get-ChildItem $bin -Filter "*.dll" | Where-Object { $_.Name -match '^(avformat|avcodec|avutil|swresample)-' } | Copy-Item -Destination $Out -Force
Write-Host "daphne_core.dll deployed to $Out"
