# Fetches the binary dependencies that are too large to commit:
#   tools\ffmpeg.exe                     - encoding and audio decoding
#   third_party\onnxruntime\{include,lib} - ONNX Runtime 1.20.1 + DirectML
#   models\*.onnx                        - speaker-embedding model
#
#   powershell -ExecutionPolicy Bypass -File scripts\fetch_deps.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Ensure-Dir($path) { New-Item -ItemType Directory -Force -Path $path | Out-Null }

# --- ffmpeg ----------------------------------------------------------------
$tools = Join-Path $root 'tools'
Ensure-Dir $tools
$ffmpegOut = Join-Path $tools 'ffmpeg.exe'

if (Test-Path $ffmpegOut) {
    Write-Host "ffmpeg already present: $ffmpegOut"
} else {
    # Prefer a copy already on this machine over a ~100 MB download.
    $candidates = @(
        'C:\tools\ffmpeg_extracted\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe',
        "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1.2-full_build\bin\ffmpeg.exe"
    )
    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $found) {
        $found = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
    }
    if ($found) {
        Copy-Item $found $ffmpegOut -Force
        Write-Host "Copied ffmpeg from $found"
    } else {
        Write-Warning @'
ffmpeg.exe was not found on this machine. Download a static Windows build from
https://github.com/BtbN/FFmpeg-Builds/releases (the win64-gpl archive) and copy
bin\ffmpeg.exe into tools\. The app can also be pointed at an existing ffmpeg
via Settings or the DVS_FFMPEG environment variable.
'@
    }
}

# --- ONNX Runtime (DirectML build) -----------------------------------------
$ortRoot = Join-Path $root 'third_party\onnxruntime'
if (Test-Path (Join-Path $ortRoot 'lib\onnxruntime.dll')) {
    Write-Host "ONNX Runtime already present."
} else {
    Ensure-Dir (Join-Path $ortRoot 'include')
    Ensure-Dir (Join-Path $ortRoot 'lib')
    $version = '1.20.1'
    $nupkg = Join-Path $env:TEMP "ort-dml-$version.nupkg"
    $url = "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/$version"
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $nupkg -UseBasicParsing

    $extract = Join-Path $env:TEMP "ort-dml-$version"
    if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
    Expand-Archive -Path $nupkg -DestinationPath $extract -Force

    Copy-Item (Join-Path $extract 'build\native\include\*') (Join-Path $ortRoot 'include') -Recurse -Force
    Copy-Item (Join-Path $extract 'runtimes\win-x64\native\onnxruntime.dll') (Join-Path $ortRoot 'lib') -Force
    Copy-Item (Join-Path $extract 'runtimes\win-x64\native\onnxruntime.lib') (Join-Path $ortRoot 'lib') -Force
    Write-Host "Installed ONNX Runtime $version"

    # DirectML.dll >= 1.15.2 is required by ORT 1.20.1's DML EP. The
    # Microsoft.AI.DirectML nupkg is ~190 MB and multi-arch; scavenging an
    # already-installed copy is far quicker and works fine.
    $dmlOut = Join-Path $ortRoot 'lib\DirectML.dll'
    if (-not (Test-Path $dmlOut)) {
        $dmlCandidates = @(
            'C:\Program Files\Microsoft Office\root\Office16\WinAppSDK\DirectML.dll',
            "$env:ProgramFiles\Adobe\Adobe Premiere Pro 2024\DirectML.dll",
            "$env:SystemRoot\System32\DirectML.dll"
        )
        $dml = $dmlCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($dml) {
            Copy-Item $dml $dmlOut -Force
            Write-Host "Copied DirectML.dll from $dml"
        } else {
            Write-Warning "DirectML.dll not found; the app will run speaker splitting on the CPU."
        }
    }
}

# --- speaker-embedding model ------------------------------------------------
$models = Join-Path $root 'models'
Ensure-Dir $models

$existing = Get-ChildItem -Path $models -Filter '*.onnx' -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "Model already present: $($existing[0].Name)"
    exit 0
}

# sherpa-onnx publishes ready-to-use ONNX exports of the WeSpeaker and
# 3D-Speaker embedding models as plain GitHub release assets, so no
# HuggingFace account or token is involved.
$base = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models'
$candidates = @(
    'wespeaker_en_voxceleb_CAM++.onnx',
    '3dspeaker_speech_campplus_sv_zh-cn_16k-common.onnx',
    'nemo_en_titanet_small.onnx'
)

foreach ($name in $candidates) {
    $url = "$base/$name"
    $out = Join-Path $models $name
    Write-Host "Trying $url"
    try {
        Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
        $size = (Get-Item $out).Length
        if ($size -lt 1MB) { throw "downloaded file is only $size bytes" }
        Write-Host "Saved $out ($([math]::Round($size/1MB,1)) MB)"
        exit 0
    } catch {
        Write-Warning "  failed: $($_.Exception.Message)"
        if (Test-Path $out) { Remove-Item $out -Force }
    }
}

Write-Warning @'
No speaker-embedding model could be downloaded.
The app still runs: speaker splitting falls back to the German/English text
check, and every line stays editable in the segment table. Drop any ONNX
speaker-embedding model into models\ to enable voice-based splitting.
'@
exit 1
