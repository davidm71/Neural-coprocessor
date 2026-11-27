<#
.SYNOPSIS
  IN2: makes the installer package (the release folder + zip) from your latest local build.

.DESCRIPTION
  Run it after building with CMake (cmake -B build -A x64, then
  cmake --build build --config Release). It collects:

    AtomicMGPU-Setup.exe                     the installer (from the build)
    mgpu-report.bat                          problem report helper (versions + SHA-256)
    nvngx.dll_mgpu_bridge.addon64            the add-on (from the build, renamed)
    mgpu.ini, gpu1.ini, ReShade2.ini         from assets\
    README.txt, LICENSE
    reshade-shaders\Shaders\mgpu_depth_tap.fx
    reshade\ReShade64.dll + ReShade-LICENSE.txt   ReShade's add-on build (optional)

  into dist\<name>\ and zips it as dist\<name>.zip - the same set the GitHub
  build makes. Hand the zip out; the user unpacks it and runs AtomicMGPU-Setup.exe.

  ReShade: downloaded from reshade.me (needs 7-Zip to unpack ReShade's setup),
  or taken from -ReShadeSetup if you already have ReShade_Setup_x.y.z_Addon.exe.
  It is only included together with its licence text. -NoReShade leaves it out
  (the installer then tells users to install ReShade themselves).

.EXAMPLE
  .\installer\make_package.ps1
.EXAMPLE
  .\installer\make_package.ps1 -ReShadeSetup C:\Downloads\ReShade_Setup_6.8.0_Addon.exe
.EXAMPLE
  .\installer\make_package.ps1 -NoReShade -Name Atomic-MGPU-Bridge-test
#>
param(
    [string]$Build = "build",
    [string]$Name = "",
    [string]$ReShadeVersion = "6.8.0",
    [string]$ReShadeSetup = "",
    [switch]$NoReShade
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Newest($dir, $filter) {
    Get-ChildItem -Path $dir -Recurse -File -Filter $filter -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

# ---- the built files ----
if (-not (Test-Path $Build)) { throw "No '$Build' folder - build first: cmake -B build -A x64; cmake --build build --config Release" }
$addon = Newest $Build "mgpu_bridge.addon64"
$setup = Newest $Build "AtomicMGPU-Setup.exe"
if (-not $addon) { throw "mgpu_bridge.addon64 not found under '$Build' - build the Release configuration first." }
if (-not $setup) { throw "AtomicMGPU-Setup.exe not found under '$Build' - rebuild with this CMakeLists.txt (target atomic_mgpu_setup)." }
Write-Host "Add-on:    $($addon.FullName)  ($($addon.LastWriteTime))"
Write-Host "Installer: $($setup.FullName)  ($($setup.LastWriteTime))"
if (($addon.LastWriteTime - $setup.LastWriteTime).Duration().TotalHours -gt 12) {
    Write-Warning "The add-on and the installer were built more than 12 hours apart - is one of them stale?"
}

# ---- the name: version, build tag, and whether it is the diagnostic build ----
if (-not $Name) {
    $dm = Get-Content "src\dllmain.cpp" -Raw
    $ver = if ($dm -match '#define MGPU_VERSION_STR "([^"]+)"') { $Matches[1] } else { "x" }
    $tag = if ($dm -match '#define MGPU_SOURCE_TAG "([^"]+)"') { $Matches[1] } else { "x" }
    $diag = ""
    $cache = Join-Path $Build "CMakeCache.txt"
    if ((Test-Path $cache) -and ((Get-Content $cache -Raw) -notmatch 'MGPU_PRODUCTION:BOOL=ON')) { $diag = "-diag" }
    $Name = "Atomic-MGPU-Bridge-$ver-$tag$diag"
}
$dist = Join-Path $root "dist"
$out  = Join-Path $dist $Name
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force -Path $out, (Join-Path $out "reshade-shaders\Shaders") | Out-Null

# ---- the release set ----
Copy-Item $setup.FullName (Join-Path $out "AtomicMGPU-Setup.exe")
Copy-Item (Join-Path $root "installer\mgpu-report.bat") (Join-Path $out "mgpu-report.bat")   # RP2: problem report helper
Copy-Item $addon.FullName (Join-Path $out "nvngx.dll_mgpu_bridge.addon64")
foreach ($f in "assets\mgpu.ini", "assets\gpu1.ini", "assets\ReShade2.ini", "assets\README.txt", "LICENSE") {
    Copy-Item $f $out
}
Copy-Item "assets\reshade-shaders\Shaders\mgpu_depth_tap.fx" (Join-Path $out "reshade-shaders\Shaders")

# ---- ReShade (add-on support build), only with its licence ----
if (-not $NoReShade) {
    $tmp = Join-Path $env:TEMP "atomic_reshade"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        if ($ReShadeSetup) {
            if (-not (Test-Path $ReShadeSetup)) { throw "ReShade setup '$ReShadeSetup' not found" }
            $rs = $ReShadeSetup
        } else {
            $rs = Join-Path $tmp "ReShade_Setup_Addon.exe"
            Write-Host "Downloading ReShade $ReShadeVersion (add-on build) from reshade.me ..."
            Invoke-WebRequest -UseBasicParsing -Uri "https://reshade.me/downloads/ReShade_Setup_${ReShadeVersion}_Addon.exe" -OutFile $rs
        }
        $7z = @((Get-Command 7z.exe -ErrorAction SilentlyContinue).Source,
                "$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") |
              Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
        if (-not $7z) { throw "7-Zip is needed to take ReShade64.dll out of ReShade's setup (install it from 7-zip.org)" }
        & $7z e -y "-o$tmp" $rs ReShade64.dll | Out-Null
        if (-not (Test-Path (Join-Path $tmp "ReShade64.dll"))) { throw "ReShade64.dll was not found inside $rs" }
        $lic = Join-Path $tmp "ReShade-LICENSE.txt"
        Invoke-WebRequest -UseBasicParsing -Uri "https://raw.githubusercontent.com/crosire/reshade/v${ReShadeVersion}/LICENSE.md" -OutFile $lic
        New-Item -ItemType Directory -Force -Path (Join-Path $out "reshade") | Out-Null
        Copy-Item (Join-Path $tmp "ReShade64.dll") (Join-Path $out "reshade\ReShade64.dll")
        Copy-Item $lic (Join-Path $out "reshade\ReShade-LICENSE.txt")
        Write-Host "Bundled ReShade $ReShadeVersion (add-on support build) with its licence."
    } catch {
        Write-Warning "ReShade NOT bundled: $($_.Exception.Message). The installer will ask users to install ReShade themselves."
        if (Test-Path (Join-Path $out "reshade")) { Remove-Item -Recurse -Force (Join-Path $out "reshade") }
    }
}

# ---- nothing from NVIDIA may ship ----
$nv = Get-ChildItem $out -Recurse -File | Where-Object { $_.Name -match '^nvngx_|^sl\.|^nvapi' }
if ($nv) { throw "NVIDIA files found in the package - they must not be redistributed: $($nv.Name -join ', ')" }

# ---- zip ----
$zip = Join-Path $dist "$Name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $out -DestinationPath $zip
Write-Host ""
Get-ChildItem $out -Recurse -File | ForEach-Object { "  " + $_.FullName.Substring($out.Length + 1) }
Write-Host ""
Write-Host "Package: $zip"
