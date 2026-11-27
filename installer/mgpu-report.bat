@echo off
rem Atomic MGPU Bridge - problem report helper (RP2).
rem Put this file in the game's exe folder (next to ReShade's dxgi.dll / the .addon64)
rem and double-click it. It writes mgpu-report.txt in the same folder: Windows build,
rem graphics cards, NVIDIA driver version, and the version and SHA-256 hash of every
rem bridge-related file. Attach mgpu-report.txt and ReShade.log to your report.
rem It only reads; nothing is changed or sent anywhere.
rem RP3: the PowerShell part is the text after the marker line at the end of this file.
setlocal
cd /d "%~dp0"
echo Collecting versions and hashes in %CD% ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$t = [IO.File]::ReadAllText('%~f0'); $m = [char]10 + '#' + 'PS-START'; $i = $t.IndexOf($m); if ($i -lt 0) { Write-Host 'Script part not found.'; exit 1 }; Invoke-Expression $t.Substring($i + $m.Length)"
echo.
echo Attach mgpu-report.txt and ReShade.log to your problem report.
pause
exit /b
#PS-START
$here = (Get-Location).Path
$out = Join-Path $here 'mgpu-report.txt'
$r = New-Object System.Collections.Generic.List[string]
$r.Add('Atomic MGPU Bridge - system report')
$r.Add('Created: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm'))
$r.Add('Folder:  ' + $here)
$r.Add('')
try {
    $os = Get-CimInstance Win32_OperatingSystem
    $r.Add('Windows: ' + $os.Caption + ' build ' + $os.BuildNumber)
} catch { $r.Add('Windows: (could not read)') }
$r.Add('')
$r.Add('Graphics cards (Windows driver version):')
try {
    foreach ($g in Get-CimInstance Win32_VideoController) { $r.Add('  ' + $g.Name + '   ' + $g.DriverVersion) }
} catch { $r.Add('  (could not read)') }
$smi = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'
$r.Add('')
if (Test-Path -LiteralPath $smi) {
    $r.Add('NVIDIA (name, driver, PCIe gen, PCIe width):')
    try {
        foreach ($l in (& $smi --query-gpu=name,driver_version,pcie.link.gen.current,pcie.link.width.current --format=csv,noheader)) { $r.Add('  ' + $l) }
    } catch { $r.Add('  nvidia-smi failed') }
} else { $r.Add('NVIDIA: nvidia-smi not found') }
$r.Add('')
$r.Add('Files (version, size, date, SHA-256):')
$pat = @('dxgi.dll','d3d12.dll','ReShade64.dll','*.addon64','ReShade.ini','ReShade2.ini','mgpu.ini','gpu1.ini','nvngx_dlssnr.dll','nvngx_dlss.dll','nvngx_dlssg.dll','nvngx_dlssd.dll','sl.*.dll','amd_fidelityfx_*.dll','mgpu_depth_tap.fx')
$dirs = @($here, (Join-Path $here 'mgpu'), (Join-Path $here 'reshade-shaders\Shaders'))
$found = 0
foreach ($d in $dirs) {
    if (-not (Test-Path -LiteralPath $d)) { continue }
    foreach ($p in $pat) {
        foreach ($x in (Get-ChildItem -Path (Join-Path $d $p) -File -ErrorAction SilentlyContinue)) {
            $found++
            $rel = $x.FullName.Substring($here.Length).TrimStart('\')
            $v = $x.VersionInfo.FileVersion
            if (-not $v) { $v = '-' }
            $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $x.FullName).Hash
            $r.Add('  ' + $rel)
            $r.Add('      version ' + $v + '   size ' + $x.Length + '   modified ' + $x.LastWriteTime.ToString('yyyy-MM-dd'))
            $r.Add('      SHA256 ' + $h)
        }
    }
}
if ($found -eq 0) { $r.Add("  none found - is this the game's exe folder?") }
$r.Add('')
$log = Join-Path $here 'ReShade.log'
if (Test-Path -LiteralPath $log) {
    $r.Add('ReShade.log: present - attach it too.')
    $m = Select-String -LiteralPath $log -Pattern '\[MGPU\]\[R141\]' | Select-Object -First 1
    if ($m) { $r.Add('  ' + $m.Line.Trim()) }
} else { $r.Add('ReShade.log: not found here.') }
$r | Set-Content -LiteralPath $out -Encoding UTF8
Write-Host ('Written: ' + $out)
