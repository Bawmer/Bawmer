"""CI: writes inference .pth files in the Applio / RVC WebUI format, to test 'voxforge-engine import-model'.
  48k: from the voice VoxForge just fine-tuned (work/checkpoints/latest.pt)
  40k: a randomly initialised RVC v2 40 kHz generator (checks other sample rates)"""
import sys
import torch
from voxforge_engine import model as M
from voxforge_engine.rvc.models import SynthesizerTrnMs768NSFsid

ckpt, out_dir = sys.argv[1], sys.argv[2]
KEYS = M._CONFIG_KEYS


def save(weights, cfg, sr_name, path):
    weights = {k: v.half() for k, v in weights.items() if not k.startswith("enc_q.")}   # Applio stores fp16, no enc_q
    torch.save({"weight": weights, "config": [cfg[k] for k in KEYS], "info": "CI test", "sr": sr_name, "f0": 1,
                "version": "v2", "vocoder": "HiFi-GAN", "embedder_model": "contentvec", "epoch": 2}, path)
    print("wrote", path)


g = torch.load(ckpt, map_location="cpu", weights_only=True)["G"]
save(g, dict(M.MODEL_ARGS), "48k", f"{out_dir}/applio48k.pth")

cfg40 = dict(M.MODEL_ARGS, sr=40000, segment_size=32, upsample_rates=[10, 10, 2, 2], upsample_kernel_sizes=[16, 16, 4, 4])
net = SynthesizerTrnMs768NSFsid(**cfg40, is_half=False)
save(net.state_dict(), cfg40, "40k", f"{out_dir}/applio40k.pth")
