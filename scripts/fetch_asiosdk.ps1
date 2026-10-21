# Downloads the Steinberg ASIO SDK into third_party/asiosdk, checked against its SHA-256 (not redistributed).
# ASIO SDK 2.3.4 is dual-licensed (Steinberg's proprietary licence or GPLv3); Daphne (GPL-3.0) uses it under GPLv3.
# Only the host-side headers (common/) are used.
param(
    [string]$Url = "https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip",
    [string]$Sha256 = "d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\asiosdk"
$zip = Join-Path $env:TEMP ("daphne_" + [IO.Path]::GetFileName($Url))
$tmp = Join-Path $env:TEMP "daphne_asiosdk_extract"
Write-Host "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
$got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
if ($got -ne $Sha256.ToLowerInvariant()) {
    Remove-Item -Force $zip
    throw "ASIO SDK: SHA-256 $got, expected $Sha256 ($Url)"
}
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
Expand-Archive -Path $zip -DestinationPath $tmp
$top = Get-ChildItem $tmp | Where-Object { Test-Path (Join-Path $_.FullName "common\iasiodrv.h") } | Select-Object -First 1
if (-not $top) { throw "unexpected ASIO SDK archive layout" }
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
Move-Item $top.FullName $dest
Set-Content -Path (Join-Path $dest "SOURCE.txt") -Value $Url
Remove-Item -Force $zip
Remove-Item -Recurse -Force $tmp
Write-Host "ASIO SDK installed to $dest"
