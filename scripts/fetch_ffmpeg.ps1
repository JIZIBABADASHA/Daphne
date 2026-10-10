# Downloads the FFmpeg shared development package (LGPL build, linked dynamically) into third_party/ffmpeg.
param(
    [string]$Url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-05-13-07/ffmpeg-n8.1.3-14-g330caae0c1-win64-lgpl-shared-8.1.zip"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\ffmpeg"
$zip = Join-Path $env:TEMP ("daphne_ffmpeg_" + [IO.Path]::GetFileName($Url))
$tmp = Join-Path $env:TEMP "daphne_ffmpeg_extract"
Write-Host "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
Expand-Archive -Path $zip -DestinationPath $tmp
$top = Get-ChildItem $tmp | Select-Object -First 1
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Move-Item $top.FullName $dest
Set-Content -Path (Join-Path $dest "SOURCE.txt") -Value $Url
Remove-Item -Force $zip
Remove-Item -Recurse -Force $tmp
Write-Host "FFmpeg installed to $dest"
