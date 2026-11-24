<#
  Installs the VoxForge Engine (neural voice training + lyrics transcription) into
  %APPDATA%\VoxForge\engine-venv, where the plug-in finds it automatically.

  Requirements: Python 3.10 - 3.12 (64-bit) on PATH ("py" launcher or "python").
  Run from a normal PowerShell:   powershell -ExecutionPolicy Bypass -File install_engine.ps1
  Options:  -Cpu      force the CPU build of PyTorch (no NVIDIA GPU)
            -Whisper  pre-download the Whisper 'small' model for offline lyrics extraction
#>
param([switch]$Cpu, [switch]$Whisper)
$ErrorActionPreference = "Stop"

$data   = Join-Path $env:APPDATA "VoxForge"
$venv   = Join-Path $data "engine-venv"
$models = Join-Path $data "Models"
$engineSrc = Resolve-Path (Join-Path $PSScriptRoot "..\engine")
New-Item -ItemType Directory -Force -Path $data, $models | Out-Null

function Find-Python {
    foreach ($cand in @("py -3.12", "py -3.11", "py -3.10", "python")) {
        try {
            $exe, $args0 = $cand.Split(" ", 2)
            $v = & $exe $args0 -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null
            if ($LASTEXITCODE -eq 0 -and $v -match "^3\.(1[0-2])$") { return $cand }
        } catch {}
    }
    throw "Python 3.10-3.12 (64-bit) not found. Install it from https://www.python.org/downloads/ and re-run."
}

Write-Host "== VoxForge Engine installer ==" -ForegroundColor Cyan
$py = Find-Python
Write-Host "Using $py"
if (-not (Test-Path (Join-Path $venv "Scripts\python.exe"))) {
    $exe, $args0 = $py.Split(" ", 2)
    & $exe $args0 -m venv $venv
}
$vpy = Join-Path $venv "Scripts\python.exe"
& $vpy -m pip install --upgrade pip wheel

$hasNvidia = $false
$gpuName = ""; $cc = 0.0
if (-not $Cpu) {
    try {
        $q = nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader 2>$null
        if ($LASTEXITCODE -eq 0 -and $q) {
            $hasNvidia = $true
            $first = ($q | Select-Object -First 1).Split(",")
            $gpuName = $first[0].Trim()
            if ($first.Count -gt 1) { [double]::TryParse($first[1].Trim(), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$cc) | Out-Null }
        } else { nvidia-smi | Out-Null; $hasNvidia = ($LASTEXITCODE -eq 0) }
    } catch {}
}
if ($hasNvidia) {
    # RTX 50 series (Blackwell, compute capability 12.x) needs a CUDA 12.8 build of PyTorch (driver 570+);
    # RTX 20/30/40 use the CUDA 12.1 build VoxForge is tested with.
    if ($cc -ge 12.0 -or $gpuName -match "RTX\s*50\d\d|RTX PRO|Blackwell") {
        Write-Host "NVIDIA $gpuName (compute $cc) - installing PyTorch 2.7.1 for CUDA 12.8" -ForegroundColor Green
        & $vpy -m pip install torch==2.7.1 torchaudio==2.7.1 --index-url https://download.pytorch.org/whl/cu128
    } else {
        Write-Host "NVIDIA $gpuName (compute $cc) - installing PyTorch 2.5.1 for CUDA 12.1" -ForegroundColor Green
        & $vpy -m pip install torch==2.5.1 torchaudio==2.5.1 --index-url https://download.pytorch.org/whl/cu121
    }
} else {
    Write-Host "Installing CPU PyTorch (training will be slow; transcription is fine)" -ForegroundColor Yellow
    & $vpy -m pip install torch==2.5.1 torchaudio==2.5.1 --index-url https://download.pytorch.org/whl/cpu
}
& $vpy -m pip install $engineSrc

$engine = Join-Path $venv "Scripts\voxforge-engine.exe"
Write-Host "Exporting the shared content encoder (HuBERT, downloads ~360 MB once)..."
& $engine export-content-encoder --models-dir $models
Write-Host "Downloading the pretrained RVC v2 48 kHz base model (~220 MB, MIT license)..."
& $engine download-base --models-dir $models

$dict = Join-Path $data "cmudict.dict"
if (-not (Test-Path $dict)) {
    Write-Host "Downloading the CMU pronouncing dictionary..."
    try { Invoke-WebRequest "https://raw.githubusercontent.com/cmusphinx/cmudict/master/cmudict.dict" -OutFile $dict }
    catch { Write-Warning "Could not download cmudict.dict (optional): $_" }
}

if ($Whisper) {
    Write-Host "Pre-downloading Whisper 'small'..."
    & $vpy -c "from faster_whisper import WhisperModel; WhisperModel('small', download_root=r'$models\whisper')"
}

& $engine check --models-dir $models
if ($hasNvidia) {
    & $vpy -c "import sys, torch; ok = torch.cuda.is_available() and float((torch.ones(64, 64, device='cuda') @ torch.ones(64, 64, device='cuda'))[0, 0]) == 64.0; print('CUDA GPU test:', torch.cuda.get_device_name(0) if ok else 'FAILED'); sys.exit(0 if ok else 1)"
    if ($LASTEXITCODE -ne 0) { Write-Warning "PyTorch cannot use the NVIDIA GPU. Update the NVIDIA driver (RTX 50 series: 570 or newer) and re-run this script." }
}
Write-Host "`nDone. Restart your DAW (or re-open VoxForge) - the engine is detected automatically." -ForegroundColor Cyan
