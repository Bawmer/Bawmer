#!/usr/bin/env bash
# Installs the VoxForge Engine into the per-user data folder (macOS/Linux development).
#   ./install_engine.sh [--cpu]
set -euo pipefail
case "$(uname -s)" in
  Darwin) DATA="$HOME/Library/Application Support/VoxForge" ;;
  *)      DATA="${XDG_CONFIG_HOME:-$HOME/.config}/VoxForge" ;;
esac
VENV="$DATA/engine-venv"
MODELS="$DATA/Models"
SRC="$(cd "$(dirname "$0")/../engine" && pwd)"
mkdir -p "$DATA" "$MODELS"

python3 -m venv "$VENV"
"$VENV/bin/python" -m pip install --upgrade pip wheel
if [[ "${1:-}" == "--cpu" ]]; then
  "$VENV/bin/python" -m pip install torch==2.5.1 torchaudio==2.5.1 --index-url https://download.pytorch.org/whl/cpu
else
  CC="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 || true)"
  if [[ -n "$CC" && "${CC%%.*}" -ge 12 ]]; then
    # RTX 50 series (Blackwell) needs a CUDA 12.8 build
    "$VENV/bin/python" -m pip install torch==2.7.1 torchaudio==2.7.1 --index-url https://download.pytorch.org/whl/cu128
  else
    "$VENV/bin/python" -m pip install torch==2.5.1 torchaudio==2.5.1
  fi
fi
"$VENV/bin/python" -m pip install "$SRC"
"$VENV/bin/voxforge-engine" export-content-encoder --models-dir "$MODELS"
"$VENV/bin/voxforge-engine" download-base --models-dir "$MODELS"
[[ -f "$DATA/cmudict.dict" ]] || curl -fsSL -o "$DATA/cmudict.dict" \
  https://raw.githubusercontent.com/cmusphinx/cmudict/master/cmudict.dict || true
"$VENV/bin/voxforge-engine" check --models-dir "$MODELS"
