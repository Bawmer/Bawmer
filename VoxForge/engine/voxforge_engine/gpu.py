"""NVIDIA GPU (CUDA) support helpers for the engine: device report, RTX speed settings and
the CUDA DLL path for faster-whisper on Windows."""
from __future__ import annotations

import os
import sys


def info() -> dict:
    """{'available', 'name', 'capability', 'vram_gb', 'torch_cuda', 'supported', 'message'}"""
    out = {"available": False, "name": None, "capability": None, "vram_gb": None,
           "torch_cuda": None, "supported": False, "message": "no NVIDIA GPU / CPU build of PyTorch"}
    try:
        import torch
    except ImportError:
        return out
    out["torch_cuda"] = torch.version.cuda
    if not torch.cuda.is_available():
        if torch.version.cuda is None:
            out["message"] = "CPU build of PyTorch - re-run install_engine.ps1 on a PC with an NVIDIA GPU"
        return out
    major, minor = torch.cuda.get_device_capability(0)
    props = torch.cuda.get_device_properties(0)
    out.update(available=True, name=torch.cuda.get_device_name(0), capability=f"{major}.{minor}",
               vram_gb=round(props.total_memory / 2**30, 1))
    # the PyTorch build must contain kernels for this GPU generation (RTX 50 = sm_120 needs CUDA 12.8 builds)
    archs = {a for a in torch.cuda.get_arch_list() if a.startswith("sm_")}
    have = max((int(a[3:]) for a in archs), default=0)
    out["supported"] = f"sm_{major}{minor}" in archs or (major * 10 + minor) <= have
    out["message"] = (f"{out['name']} ({out['vram_gb']} GB, sm_{major}{minor}), CUDA {torch.version.cuda}"
                      if out["supported"] else
                      f"{out['name']} (sm_{major}{minor}) is newer than this PyTorch build supports "
                      f"({', '.join(sorted(archs))}). Re-run install_engine.ps1 to get a CUDA 12.8 build.")
    return out


def configure_for_training() -> None:
    """RTX (Ampere and newer): TF32 tensor cores for matmul/conv, cuDNN autotuning."""
    import torch
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True
    torch.backends.cudnn.benchmark = True


def add_cuda_dll_path() -> None:
    """faster-whisper (CTranslate2) needs cuBLAS / cuDNN; CUDA builds of PyTorch ship them in torch/lib."""
    if sys.platform != "win32":
        return
    try:
        import torch
        lib = os.path.join(os.path.dirname(torch.__file__), "lib")
        if os.path.isdir(lib):
            os.add_dll_directory(lib)
            os.environ["PATH"] = lib + os.pathsep + os.environ.get("PATH", "")
    except Exception:  # noqa: BLE001
        pass
