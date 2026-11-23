"""Speaker-independent phonetic content features ("units").

We use the output of transformer layer 12 of HuBERT-Base (torchaudio bundle of
fairseq's hubert_base_ls960), 768-dim at 50 frames/s on 16 kHz audio - exactly the
features RVC v2 was trained on, so the pretrained RVC v2 base model can be used.
"""
from __future__ import annotations

import os

import numpy as np

LAYER = 12
UNIT_DIM = 768


class ContentEncoder:
    def __init__(self, device: str = "cpu"):
        import torch
        import torchaudio
        bundle = torchaudio.pipelines.HUBERT_BASE
        self.model = bundle.get_model().to(device).eval()
        self.device = device
        self.torch = torch

    def units(self, x16k: np.ndarray) -> np.ndarray:
        """[N] float32 @16k -> [T, 768]."""
        torch = self.torch
        with torch.inference_mode():
            wav = torch.from_numpy(x16k.astype(np.float32))[None].to(self.device)
            feats, _ = self.model.extract_features(wav, num_layers=LAYER)
            return feats[-1][0].float().cpu().numpy()


def export_onnx(path: str, device: str = "cpu", opset: int = 17) -> str:
    """Exports the content encoder: input 'audio' [1, N] -> output 'units' [1, T, 768]."""
    import torch
    import torchaudio

    class Wrapper(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.m = torchaudio.pipelines.HUBERT_BASE.get_model().eval()

        def forward(self, audio):
            feats, _ = self.m.extract_features(audio, num_layers=LAYER)
            return feats[-1]

    model = Wrapper().to(device).eval()
    dummy = torch.zeros(1, 16000 * 3, device=device)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    from .model import onnx_export
    onnx_export(model, (dummy,), tmp, input_names=["audio"], output_names=["units"],
                dynamic_axes={"audio": {1: "samples"}, "units": {1: "frames"}},
                opset_version=opset, do_constant_folding=True)
    os.replace(tmp, path)
    return path
