# Builds DialogVideoStudio-Portable.exe: one self-contained file that needs no
# installer, no Qt, no Visual C++ redistributable, and no ffmpeg on PATH.
#
#   .\packaging\build_portable.ps1
#
# Output: portable\DialogVideoStudio-Portable.exe

[CmdletBinding()]
param(
    [string]$SevenZip = 'C:\Program Files\7-Zip'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pkg  = $PSScriptRoot
$out  = Join-Path $root 'portable'
$work = Join-Path $env:TEMP ("dvs_portable_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$stage = Join-Path $work 'DialogVideoStudio'

function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }

$distDir = Join-Path $root 'dist'
$appExe  = Join-Path $distDir 'DialogVideoStudio.exe'
if (-not (Test-Path $appExe)) { throw "not found: $appExe  (build the app into dist\ first)" }

# --- 1. stage the payload ---------------------------------------------------
Step 'Staging payload'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
& robocopy $distDir $stage /E /NFL /NDL /NJH /NJS /NP /MT:8 `
    /XF '*.pdb' '*.ilk' '*.exp' '*.lib' 'gui_log.txt' | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

# The Visual C++ runtime must travel with the app; dist\ does not carry it, so
# on a clean machine the exe would refuse to start without the redistributable.
Step 'Adding Visual C++ runtime'
$crt = Get-ChildItem 'C:\Program Files (x86)\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT' -Directory -ErrorAction SilentlyContinue |
       Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw 'Microsoft.VC143.CRT redist folder not found' }
Copy-Item (Join-Path $crt.FullName '*.dll') $stage -Force

$payloadMB = [math]::Round((Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "    payload: $payloadMB MB"

# --- 2. compile the launcher ------------------------------------------------
Step 'Compiling launcher'
Copy-Item (Join-Path $SevenZip '7z.exe') $pkg -Force
Copy-Item (Join-Path $SevenZip '7z.dll') $pkg -Force

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'no MSVC toolchain found' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

New-Item -ItemType Directory -Force -Path $work | Out-Null
$launcherExe = Join-Path $work 'launcher.exe'
$bat = Join-Path $work 'build_launcher.bat'
@"
call "$vcvars" >nul
cd /d "$pkg"
rc /nologo /fo "$work\launcher.res" launcher.rc || exit /b 1
cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE launcher.cpp "$work\launcher.res" ^
   /Fe:"$launcherExe" /Fo:"$work\\" /link /SUBSYSTEM:WINDOWS || exit /b 1
"@ | Set-Content -Path $bat -Encoding ASCII
& cmd /c "`"$bat`""
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $launcherExe)) { throw 'launcher build failed' }

Remove-Item (Join-Path $pkg '7z.exe'), (Join-Path $pkg '7z.dll') -Force -ErrorAction SilentlyContinue

# --- 3. compress ------------------------------------------------------------
Step 'Compressing payload'
$archive = Join-Path $work 'payload.7z'
& (Join-Path $SevenZip '7z.exe') a -t7z -m0=lzma2 -mx=9 -md=256m -mfb=273 -mmt=on -ms=on $archive (Join-Path $stage '*') | Out-Null
if ($LASTEXITCODE -ne 0) { throw '7z compression failed' }

# --- 4. launcher + archive = one file ---------------------------------------
Step 'Assembling single exe'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$final = Join-Path $out 'DialogVideoStudio-Portable.exe'
$fs = [IO.File]::Open($final, 'Create')
foreach ($part in @($launcherExe, $archive)) {
    $in = [IO.File]::OpenRead($part); $in.CopyTo($fs, 1MB); $in.Close()
}
$fs.Close()

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
$sizeMB = [math]::Round((Get-Item $final).Length / 1MB, 1)
Write-Host ""
Write-Host "Done: $final  ($sizeMB MB)" -ForegroundColor Green
