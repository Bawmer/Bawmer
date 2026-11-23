# PyInstaller spec: builds dist/voxforge-engine/voxforge-engine.exe (one-folder build)
#   pip install pyinstaller && pyinstaller voxforge-engine.spec
from PyInstaller.utils.hooks import collect_all

datas, binaries, hidden = [], [], []
for pkg in ("torch", "torchaudio", "faster_whisper", "ctranslate2", "onnxruntime", "pyworld", "soundfile"):
    d, b, h = collect_all(pkg)
    datas += d; binaries += b; hidden += h

a = Analysis(["run_engine.py"], pathex=["."], binaries=binaries, datas=datas,
             hiddenimports=hidden + ["voxforge_engine.train", "voxforge_engine.transcribe", "voxforge_engine.content",
                            "voxforge_engine.model", "voxforge_engine.rvc.models", "voxforge_engine.rvc.models_onnx"],
             excludes=["tkinter", "matplotlib", "IPython"])
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="voxforge-engine", console=True)
coll = COLLECT(exe, a.binaries, a.datas, name="voxforge-engine")
