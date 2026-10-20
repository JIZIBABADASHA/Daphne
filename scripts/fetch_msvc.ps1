# Downloads a portable MSVC into -Dest: the x64 compiler and linker, the C/C++ runtime libraries and the Windows SDK
# headers, libraries and tools (rc, mt). The packages come from Microsoft's Visual Studio channel, each one checked
# against the SHA-256 in its manifest (the way portable-msvc.py does it). Nothing is installed or registered, and no
# administrator rights are needed: the MSIs are only unpacked (msiexec /a). For build machines without Visual Studio;
# its use falls under the Visual Studio Build Tools licence.
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [string]$Channel = "https://aka.ms/vs/17/release/channel"
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression.FileSystem

$dl = Join-Path $env:TEMP "daphne_msvc_download"
if (Test-Path $Dest) {
    # the compiler's PDB server outlives a build and keeps its DLLs open
    $full = (Resolve-Path $Dest).Path
    Get-Process mspdbsrv -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($full, [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }
    Remove-Item -Recurse -Force $Dest
}
if (Test-Path $dl) { Remove-Item -Recurse -Force $dl }
New-Item -ItemType Directory -Force $Dest, $dl | Out-Null
$Dest = (Resolve-Path $Dest).Path

function Get-File($url, $file, $sha256) {
    Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing
    $got = (Get-FileHash -Algorithm SHA256 $file).Hash
    if ($sha256 -and $got -ne $sha256.ToUpperInvariant()) { throw "MSVC: SHA-256 $got, expected $sha256 ($url)" }
}

# the channel names the Visual Studio manifest, which lists every package and its payloads
Write-Host "Reading the Visual Studio channel $Channel"
$chFile = Join-Path $dl "channel.json"
Get-File $Channel $chFile $null
$item = (Get-Content -Raw -Encoding UTF8 $chFile | ConvertFrom-Json).channelItems |
    Where-Object { $_.id -eq "Microsoft.VisualStudio.Manifests.VisualStudio" } | Select-Object -First 1
$manFile = Join-Path $dl "vs.vsman"
# (the channel's sha256 for the manifest is not that of the file served; the packages below are each checked)
Get-File $item.payloads[0].url $manFile $null
$packages = @{}
foreach ($p in (Get-Content -Raw -Encoding UTF8 $manFile | ConvertFrom-Json).packages) {
    $id = $p.id.ToLowerInvariant()
    if (-not $packages.ContainsKey($id)) { $packages[$id] = New-Object System.Collections.ArrayList }
    [void]$packages[$id].Add($p)
}
function Get-Package($id) {
    $p = $packages[$id.ToLowerInvariant()] | Where-Object { -not $_.language -or $_.language -eq "en-US" } | Select-Object -First 1
    if (-not $p) { throw "MSVC: package $id not in the Visual Studio manifest" }
    return $p
}

# the newest MSVC and Windows SDK (components Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64, ...Windows11SDK.26100)
$msvc = $packages.Keys | ForEach-Object { if ($_ -match '^microsoft\.visualstudio\.component\.vc\.(\d+\.\d+\.\d+\.\d+)\.x86\.x64$') { $Matches[1] } } |
    Sort-Object { [version]$_ } | Select-Object -Last 1
$sdkComponent = $packages.Keys | Where-Object { $_ -match '^microsoft\.visualstudio\.component\.windows1[01]sdk\.\d+$' } |
    Sort-Object { [int]($_ -split '\.')[-1] } | Select-Object -Last 1
if (-not $msvc -or -not $sdkComponent) { throw "MSVC: no MSVC or Windows SDK component in the Visual Studio manifest" }
$sdk = Get-Package ((Get-Package $sdkComponent).dependencies.PSObject.Properties | Select-Object -First 1).Name
Write-Host "MSVC $msvc, $($sdk.id)"

# MSVC: VSIX packages (zip files), whose Contents\ folder is the layout under the Visual Studio folder (VC\Tools\MSVC\...);
# the import libraries of the DLL runtime (/MD: msvcrt.lib, vcruntime.lib, msvcprt.lib) are in CRT.x64.Store
foreach ($id in "Microsoft.VC.$msvc.Tools.HostX64.TargetX64.base", "Microsoft.VC.$msvc.Tools.HostX64.TargetX64.Res.base",
                "Microsoft.VC.$msvc.CRT.Headers.base", "Microsoft.VC.$msvc.CRT.x64.Desktop.base", "Microsoft.VC.$msvc.CRT.x64.Store.base") {
    foreach ($payload in (Get-Package $id).payloads) {
        Write-Host "Downloading $($payload.fileName)"
        $file = Join-Path $dl $payload.sha256
        Get-File $payload.url $file $payload.sha256
        $zip = [IO.Compression.ZipFile]::OpenRead($file)
        try {
            foreach ($e in $zip.Entries) {
                if (-not $e.FullName.StartsWith("Contents/") -or $e.FullName.EndsWith("/")) { continue }
                $out = Join-Path $Dest ([Uri]::UnescapeDataString($e.FullName.Substring(9)))
                New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $out, $true)
            }
        } finally { $zip.Dispose() }
        Remove-Item -Force $file
    }
}

# Windows SDK: MSIs and the CABs they name, unpacked into Dest\Windows Kits\10
$sdkDir = Join-Path $dl "sdk"
New-Item -ItemType Directory -Force $sdkDir | Out-Null
function Get-SdkPayload($name) {
    $payload = $sdk.payloads | Where-Object { $_.fileName -eq "Installers\$name" } | Select-Object -First 1
    if (-not $payload) { throw "MSVC: $name not in $($sdk.id)" }
    $file = Join-Path $sdkDir $name
    Get-File $payload.url $file $payload.sha256
    return $file
}
$msis = foreach ($name in "Windows SDK for Windows Store Apps Tools-x86_en-us.msi",        # rc.exe, mt.exe
                          "Windows SDK for Windows Store Apps Headers-x86_en-us.msi",
                          "Windows SDK for Windows Store Apps Headers OnecoreUap-x86_en-us.msi",   # shared\winapifamily.h, ...
                          "Windows SDK OnecoreUap Headers x86-x86_en-us.msi",
                          "Windows SDK Desktop Headers x86-x86_en-us.msi",
                          "Windows SDK Desktop Headers x64-x86_en-us.msi",
                          "Windows SDK for Windows Store Apps Libs-x86_en-us.msi",
                          "Windows SDK Desktop Libs x64-x86_en-us.msi",
                          "Universal CRT Headers Libraries and Sources-x86_en-us.msi") {
    Write-Host "Downloading $name"
    Get-SdkPayload $name
}
# an MSI names its external CABs (32 hex digits + .cab) in its tables
$latin1 = [Text.Encoding]::GetEncoding(28591)
$cabs = $msis | ForEach-Object { [regex]::Matches($latin1.GetString([IO.File]::ReadAllBytes($_)), '[0-9a-f]{32}\.cab') | ForEach-Object { $_.Value } } |
    Sort-Object -Unique
foreach ($cab in $cabs) { Write-Host "Downloading $cab"; Get-SdkPayload $cab | Out-Null }
foreach ($msi in $msis) {
    Write-Host "Unpacking $([IO.Path]::GetFileName($msi))"
    $p = Start-Process msiexec.exe -ArgumentList "/a", "`"$msi`"", "/quiet", "/qn", "TARGETDIR=`"$Dest`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "MSVC: msiexec /a $msi ended with $($p.ExitCode)" }
    Remove-Item -Force (Join-Path $Dest ([IO.Path]::GetFileName($msi))) -ErrorAction SilentlyContinue
}

# only x64 desktop builds are made with it: the other architectures, the runtime PDBs, the redistributables and sources
# that come in the same packages go (about a third of the size), and so does the compiler's telemetry helper
$kits = Join-Path $Dest "Windows Kits\10"
$drop = @(Get-ChildItem (Join-Path $kits "Lib\*\*\*") -Directory | Where-Object { $_.Name -ne "x64" }) +
        @(Get-ChildItem (Join-Path $kits "bin\*\*") -Directory | Where-Object { $_.Name -ne "x64" }) +
        @(Get-ChildItem $kits -Directory | Where-Object { $_.Name -in "Redist", "Source", "Catalogs", "DesignTime" }) +
        @(Get-ChildItem (Join-Path $Dest "VC\Tools\MSVC\*\lib\x64\*") -Directory)
$drop | Remove-Item -Recurse -Force
Get-ChildItem (Join-Path $Dest "VC\Tools\MSVC") -Recurse -Include *.pdb, vctip.exe -File | Remove-Item -Force
Remove-Item -Recurse -Force $dl
# written last: a folder without it is an interrupted download (build_for_miku.ps1 fetches again)
Set-Content -Path (Join-Path $Dest "SOURCE.txt") -Value "MSVC $msvc, $($sdk.id), from $Channel"
Write-Host "MSVC installed to $Dest"
