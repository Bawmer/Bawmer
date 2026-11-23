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
  "$VENV/bin/python" -m pip install torch torchaudio --index-url https://download.pytorch.org/whl/cpu
else
  "$VENV/bin/python" -m pip install torch torchaudio
fi
"$VENV/bin/python" -m pip install "$SRC"
"$VENV/bin/voxforge-engine" export-content-encoder --models-dir "$MODELS"
[[ -f "$DATA/cmudict.dict" ]] || curl -fsSL -o "$DATA/cmudict.dict" \
  https://raw.githubusercontent.com/cmusphinx/cmudict/master/cmudict.dict || true
"$VENV/bin/voxforge-engine" check --models-dir "$MODELS"
