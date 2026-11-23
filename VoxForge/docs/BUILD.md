# Building VoxForge

VoxForge has two parts:

| Part | Language | Output |
|---|---|---|
| Plug-in | C++17, JUCE 8 | `VoxForge.vst3` (Windows x64), optional standalone `VoxForge.exe` |
| Engine | Python 3.10–3.12, PyTorch | `voxforge-engine` (venv console script or PyInstaller folder) |

The plug-in works without the engine; it uses the DSP engine and Lite clones.
The engine adds neural training and lyrics transcription.

## 1. Windows x64 VST3

### Prerequisites

- Visual Studio 2022 with the **Desktop development with C++** workload
- CMake 3.22 or newer, and Git
- Internet access for the first configure: CMake downloads JUCE 8.0.6, and the build
  script downloads ONNX Runtime

### One command

```powershell
cd VoxForge
powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1            # neural backend on
powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1 -NoOnnx    # DSP engine only
powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1 -Install   # + copy to Common Files\VST3 (admin)
```

The script downloads ONNX Runtime, configures and builds Release, runs the engine tests and
the host smoke test, and prints the path of `VoxForge.vst3`.

### Manual CMake

```powershell
# optional: ONNX Runtime CPU package (include/ + lib/), e.g. onnxruntime-win-x64-1.20.1.zip from the ORT releases
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DVOXFORGE_WITH_ONNX=ON -DONNXRUNTIME_ROOT=C:/libs/onnxruntime-win-x64-1.20.1 `
      -DVOXFORGE_BUILD_TESTS=ON
cmake --build build --config Release --parallel
```

| CMake option | Default | Meaning |
|---|---|---|
| `VOXFORGE_WITH_ONNX` | OFF | Neural voice playback through ONNX Runtime |
| `ONNXRUNTIME_ROOT` | – | ORT package root (`include/`, `lib/`). NuGet layouts (`build/native/include`, `runtimes/win-x64/native`) also work |
| `VOXFORGE_BUILD_TESTS` | OFF | `VoxForgeTests`, `VoxForgeHostTest` and `VoxForgeUiSnapshot` |
| `VOXFORGE_STANDALONE` | ON | Also build the standalone app |
| `JUCE_DIR` | – | Use a local JUCE checkout instead of downloading one |

Outputs:

- `build/VoxForge_artefacts/Release/VST3/VoxForge.vst3`. The ORT DLLs are copied into
  `Contents/x86_64-win/` automatically.
- `build/VoxForge_artefacts/Release/Standalone/VoxForge.exe`

Notes on the Windows build:

- **Static CRT** (`/MT`): no VC++ redistributable is needed.
- **ONNX Runtime is delay-loaded.** At first use the plug-in loads `onnxruntime.dll` from its
  own folder by full path. This matters because Windows ships an old `onnxruntime.dll` in
  `System32`, and hosts do not search plug-in folders. The ORT C++ API is initialised by hand
  (`ORT_API_MANUAL_INIT`) after that load.
- Uses VST3 SDK through JUCE. `JUCE_VST3_CAN_REPLACE_VST2=0`.

### CI

`.github/workflows/voxforge-windows.yml` builds on `windows-2022`, runs both test programs, and
uploads `VoxForge-win-x64` (VST3, standalone, ORT DLLs, engine sources, installer script,
manual, and test renders) as a build artifact. Trigger it from the *Actions* tab with
*Run workflow*, or push a change under `VoxForge/`.

### Installer

Install [Inno Setup 6](https://jrsoftware.org/isinfo.php), build Release, then run:

```powershell
iscc installer\VoxForge.iss      # -> installer\Output\VoxForge-Setup-x64.exe
```

The installer puts the VST3 in `C:\Program Files\Common Files\VST3`. It can also install the
standalone app, the engine sources, and a start-menu shortcut that runs `install_engine.ps1`.

## 2. Engine

### Developer / user install (recommended)

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install_engine.ps1    # Windows
./scripts/install_engine.sh                                             # macOS / Linux
```

This creates `%APPDATA%\VoxForge\engine-venv`, installs PyTorch (CUDA when an NVIDIA GPU is
found) and the engine package, exports `Models\content_encoder.onnx`, and downloads
`cmudict.dict`. The plug-in then finds `engine-venv\Scripts\voxforge-engine.exe` on its own.

### Stand-alone engine executable (PyInstaller)

```powershell
cd engine
python -m venv .venv; .venv\Scripts\activate
pip install torch torchaudio --index-url https://download.pytorch.org/whl/cu121
pip install -r requirements.txt pyinstaller
pyinstaller voxforge-engine.spec
# copy dist\voxforge-engine\ to %APPDATA%\VoxForge\engine\  (or VoxForge.vst3\Contents\Resources\engine\)
```

The plug-in looks for the engine in this order: the *Engine command* setting, then
`VoxForge.vst3/Contents/Resources/engine/`, `C:\Program Files\VoxForge\engine\`,
`%APPDATA%\VoxForge\engine\`, and `%APPDATA%\VoxForge\engine-venv\Scripts\`.

### Engine CLI

```text
voxforge-engine check --models-dir DIR
voxforge-engine train --voice-dir DIR --models-dir DIR [--samples ...] [--epochs N] [--quality fast|standard|high] [--gpu]
voxforge-engine train --pretrain --data-dir CORPUS --voice-dir WORK --models-dir DIR --epochs N --gpu
voxforge-engine transcribe --audio vocal.wav [--language auto|en|...] [--model small|medium|large-v3] [--gpu]
voxforge-engine export-content-encoder --models-dir DIR
```

Every command prints JSON lines (`progress`, `log`, `result` or `error`). See `protocol.py`.

### Base model for high-quality neural voices

Fine-tuning from a model pre-trained on many singers gives far better clones from 10–30
minutes of your voice than training from scratch.

1. Collect a multi-singer corpus of dry singing. Check each dataset's licence for your
   intended use. Typical choices are OpenSinger, M4Singer, Opencpop, NUS-48E and VocalSet,
   plus your own recordings. Aim for 20 h or more.
2. Train the base model. This takes days on one GPU and is resumable:
   ```powershell
   voxforge-engine train --pretrain --data-dir D:\corpus --voice-dir D:\base-work `
                         --models-dir "$env:APPDATA\VoxForge\Models" --epochs 1000 --quality high --gpu
   ```
3. The result is written to `Models\base\base.pt`. Every later voice training starts from it
   automatically.

## 3. Linux / macOS development build and tests

The engine C++ code is cross-platform. These steps are how the project is tested
without a Windows machine:

```bash
sudo apt install libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev \
                 libxcomposite-dev libfreetype-dev mesa-common-dev ninja-build xvfb
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVOXFORGE_BUILD_TESTS=ON
cmake --build build -j
./build/VoxForgeTests_artefacts/Release/VoxForgeTests out/          # engine tests + example renders
xvfb-run -a ./build/VoxForgeHostTest_artefacts/Release/VoxForgeHostTest \
     build/VoxForge_artefacts/Release/VST3/VoxForge.vst3            # loads the VST3 like a DAW
xvfb-run -a ./build/VoxForgeUiSnapshot_artefacts/Release/VoxForgeUiSnapshot docs/images   # UI screenshots
```

What the tests cover:

- **Engine tests**: G2P and syllabification; phoneme timeline (onsets before the beat);
  synthesis speed and validity; resampling to 44.1, 88.2, 96, 176.4 and 192 kHz; the live voice
  at 192 kHz; Vocal-to-MIDI accuracy (at least 80 % of notes with correct pitch and timing);
  Lite-clone analysis recovering a known vocal-tract scale and vibrato; transparency of the
  spectral correction; state round trip; and mapping recognised words onto detected notes.
- **Host test**: scans the `.vst3`, checks that it is an instrument with stereo output and MIDI
  input, runs the live voice at six sample rates, renders offline and plays back through the
  host transport, saves and restores state, and opens the editor.

## 4. Source layout

```text
VoxForge/
  CMakeLists.txt
  Source/
    PluginProcessor.*      host integration, MIDI capture, playback, state
    PluginEditor.*         window, header, tabs
    Core/                  notes/project model, phonemes, G2P + syllabifier, timeline + pitch curves
    DSP/                   formant synth (offline + live), YIN pitch, Vocal-to-MIDI, spectral tools, resampling
    Engine/                presets, Lite clone analysis, ONNX backend, render engine, stem analysis, engine client
    UI/                    look & feel, piano roll, four tabs
  engine/                  Python engine (training, ONNX export, transcription)
  tests/                   engine tests, VST3 host smoke test, UI snapshots
  scripts/                 Windows build, engine installers
  installer/               Inno Setup script
  docs/                    manual, build guide, architecture, screenshots
```
