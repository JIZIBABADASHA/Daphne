# Downloads the Steinberg ASIO SDK into third_party/asiosdk (Steinberg's licence: not redistributed).
# Only the host-side headers (common/) are used.
param(
    [string]$Url = "https://download.steinberg.net/sdk_downloads/asiosdk_2.3.3_2019-06-14.zip"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\asiosdk"
$zip = Join-Path $env:TEMP ("daphne_" + [IO.Path]::GetFileName($Url))
$tmp = Join-Path $env:TEMP "daphne_asiosdk_extract"
Write-Host "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
Expand-Archive -Path $zip -DestinationPath $tmp
$top = Get-ChildItem $tmp | Where-Object { Test-Path (Join-Path $_.FullName "common\iasiodrv.h") } | Select-Object -First 1
if (-not $top) { throw "unexpected ASIO SDK archive layout" }
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Move-Item $top.FullName $dest
Set-Content -Path (Join-Path $dest "SOURCE.txt") -Value $Url
Remove-Item -Force $zip
Remove-Item -Recurse -Force $tmp
Write-Host "ASIO SDK installed to $dest"
