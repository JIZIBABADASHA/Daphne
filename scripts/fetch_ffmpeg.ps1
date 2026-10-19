# Downloads the FFmpeg shared development package (LGPL build, linked dynamically) into third_party/ffmpeg.
# The package is BtbN's ffmpeg-n8.1.3-14-g330caae0c1-win64-lgpl-shared-8.1 (autobuild-2026-10-05-13-07), kept as a
# release of this repository because BtbN removes old autobuilds; it is checked against its SHA-256.
param(
    [string]$Url = "https://github.com/cychang-ykc/Daphne/releases/download/ffmpeg-n8.1.3-win64/ffmpeg-n8.1.3-14-g330caae0c1-win64-lgpl-shared-8.1.zip",
    [string]$Sha256 = "df8e52990dbeb688c118484cddcc7bd0840db241b19113f3e9b5da142db598d5"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\ffmpeg"
$zip = Join-Path $env:TEMP ("daphne_ffmpeg_" + [IO.Path]::GetFileName($Url))
$tmp = Join-Path $env:TEMP "daphne_ffmpeg_extract"
Write-Host "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
$got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
if ($Sha256 -and $got -ne $Sha256.ToLowerInvariant()) {
    Remove-Item -Force $zip
    throw "FFmpeg package: SHA-256 $got, expected $Sha256 ($Url)"
}
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
Expand-Archive -Path $zip -DestinationPath $tmp
$top = Get-ChildItem $tmp | Select-Object -First 1
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Move-Item $top.FullName $dest
Set-Content -Path (Join-Path $dest "SOURCE.txt") -Value $Url
Remove-Item -Force $zip
Remove-Item -Recurse -Force $tmp
Write-Host "FFmpeg installed to $dest"
