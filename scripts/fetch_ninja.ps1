# Downloads Ninja (ninja-build's release) into -Dest, checked against its SHA-256. build_for_miku.ps1 uses it with a
# portable MSVC (fetch_msvc.ps1), which has no Visual Studio for CMake's Visual Studio generator.
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [string]$Url = "https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip",
    [string]$Sha256 = "f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a"
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$zip = Join-Path $env:TEMP "daphne_ninja-win.zip"
Write-Host "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
$got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
if ($Sha256 -and $got -ne $Sha256.ToLowerInvariant()) {
    Remove-Item -Force $zip
    throw "Ninja: SHA-256 $got, expected $Sha256 ($Url)"
}
if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
Expand-Archive -Path $zip -DestinationPath $Dest
Remove-Item -Force $zip
Write-Host "Ninja installed to $Dest"
