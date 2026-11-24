<#
  Builds VoxForge.vst3 (Windows x64, Release) with the neural backend.

  Prerequisites: Visual Studio 2022 (Desktop C++ workload), CMake >= 3.22, Git.
  Usage (from any PowerShell):
      powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1            # with ONNX (neural)
      powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1 -NoOnnx    # DSP engine only
      ... -Install     also copies VoxForge.vst3 to C:\Program Files\Common Files\VST3 (run as admin)
#>
param([switch]$NoOnnx, [switch]$Install, [string]$OrtVersion = "1.20.1", [string]$DmlVersion = "1.15.2")
$ErrorActionPreference = "Stop"

$root  = Resolve-Path (Join-Path $PSScriptRoot "..")
$build = Join-Path $root "build"
$deps  = Join-Path $root "build-deps"
New-Item -ItemType Directory -Force -Path $build, $deps | Out-Null

$cmakeArgs = @("-S", $root, "-B", $build, "-G", "Visual Studio 17 2022", "-A", "x64", "-DVOXFORGE_BUILD_TESTS=ON")

if (-not $NoOnnx) {
    # DirectML build of ONNX Runtime: neural rendering on the GPU (RTX / any DirectX 12 GPU), CPU fallback
    $ortDir = Join-Path $deps "onnxruntime-dml-$OrtVersion"
    if (-not (Test-Path $ortDir)) {
        $zip = Join-Path $deps "ort.zip"
        $url = "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/$OrtVersion"
        Write-Host "Downloading ONNX Runtime $OrtVersion with DirectML (NuGet) ..."
        Invoke-WebRequest $url -OutFile $zip
        Expand-Archive $zip -DestinationPath $ortDir -Force
        $dmlZip = Join-Path $deps "dml.zip"
        Invoke-WebRequest "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/$DmlVersion" -OutFile $dmlZip
        Expand-Archive $dmlZip -DestinationPath (Join-Path $deps "directml") -Force
        Copy-Item (Join-Path $deps "directml\bin\x64-win\DirectML.dll") (Join-Path $ortDir "runtimes\win-x64\native\")
    }
    $cmakeArgs += @("-DVOXFORGE_WITH_ONNX=ON", "-DONNXRUNTIME_ROOT=$ortDir")
}

cmake @cmakeArgs
cmake --build $build --config Release --parallel

$vst3 = Join-Path $build "VoxForge_artefacts\Release\VST3\VoxForge.vst3"
Write-Host "`nRunning engine tests..."
& (Join-Path $build "VoxForgeTests_artefacts\Release\VoxForgeTests.exe") (Join-Path $build "test-output")
Write-Host "Running host smoke test..."
& (Join-Path $build "VoxForgeHostTest_artefacts\Release\VoxForgeHostTest.exe") $vst3

if ($Install) {
    $dest = Join-Path $env:CommonProgramFiles "VST3"
    Copy-Item -Recurse -Force $vst3 $dest
    Write-Host "Installed to $dest\VoxForge.vst3"
}
Write-Host "`nBuilt: $vst3" -ForegroundColor Green
