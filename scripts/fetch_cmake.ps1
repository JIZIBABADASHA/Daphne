# Downloads CMake (Kitware's portable zip) into -Dest, checked against the SHA-256 list of the same release.
# For build machines without CMake 3.25+ (on PATH or in Visual Studio).
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [string]$Version = "3.31.6"
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$base = "https://github.com/Kitware/CMake/releases/download/v$Version"
$name = "cmake-$Version-windows-x86_64.zip"
$zip = Join-Path $env:TEMP "daphne_$name"
$tmp = Join-Path $env:TEMP "daphne_cmake_extract"
Write-Host "Downloading $base/$name"
Invoke-WebRequest -Uri "$base/$name" -OutFile $zip -UseBasicParsing
$sums = (Invoke-WebRequest -Uri "$base/cmake-$Version-SHA-256.txt" -UseBasicParsing).Content
if ($sums -is [byte[]]) { $sums = [Text.Encoding]::ASCII.GetString($sums) }
$want = ($sums -split "`n" | Where-Object { $_ -match "^([0-9a-f]{64})\s+$([regex]::Escape($name))\s*$" } | ForEach-Object { $Matches[1] }) | Select-Object -First 1
$got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
if (-not $want -or $got -ne $want) {
    Remove-Item -Force $zip
    throw "CMake: SHA-256 $got, expected $want ($name)"
}
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
Expand-Archive -Path $zip -DestinationPath $tmp
$top = Get-ChildItem $tmp | Select-Object -First 1
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
New-Item -ItemType Directory -Force (Split-Path -Parent $Dest) | Out-Null
Move-Item $top.FullName $Dest
Remove-Item -Force $zip
Remove-Item -Recurse -Force $tmp
Write-Host "CMake installed to $Dest"
