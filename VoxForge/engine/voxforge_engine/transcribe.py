"""Offline lyrics extraction with word timestamps (faster-whisper, falls back to openai-whisper)."""
from __future__ import annotations

import os

from . import protocol as P


def transcribe(args) -> dict:
    language = None if args.language in (None, "", "auto") else args.language
    model_name = args.model or "small"
    download_root = os.path.join(args.models_dir, "whisper") if args.models_dir else None

    try:
        from faster_whisper import WhisperModel
    except ImportError:
        WhisperModel = None

    if WhisperModel is not None:
        device = "cuda" if args.gpu and _cuda() else "cpu"
        compute = "float16" if device == "cuda" else "int8"
        P.progress(0.05, f"Loading Whisper '{model_name}' ({device})", force=True)
        model = WhisperModel(model_name, device=device, compute_type=compute, download_root=download_root)
        segments, info = model.transcribe(args.audio, language=language, word_timestamps=True, vad_filter=True,
                                          beam_size=5, condition_on_previous_text=True)
        words, text = [], []
        duration = max(1e-6, float(getattr(info, "duration", 0.0) or 1.0))
        for seg in segments:
            text.append(seg.text.strip())
            for w in seg.words or []:
                words.append({"word": w.word.strip(), "start": float(w.start), "end": float(w.end),
                              "probability": float(getattr(w, "probability", 1.0))})
            P.progress(0.1 + 0.85 * min(1.0, seg.end / duration), f"{seg.end:.0f}/{duration:.0f} s")
        return {"language": info.language, "text": " ".join(text), "words": words}

    try:
        import whisper  # openai-whisper
    except ImportError as e:
        raise RuntimeError("No speech recogniser installed. Run:  pip install faster-whisper") from e

    P.progress(0.05, f"Loading openai-whisper '{model_name}'", force=True)
    model = whisper.load_model(model_name, download_root=download_root)
    res = model.transcribe(args.audio, language=language, word_timestamps=True)
    words = [{"word": w["word"].strip(), "start": float(w["start"]), "end": float(w["end"])}
             for s in res.get("segments", []) for w in s.get("words", [])]
    return {"language": res.get("language", ""), "text": res.get("text", "").strip(), "words": words}


def _cuda() -> bool:
    try:
        import torch
        return torch.cuda.is_available()
    except ImportError:
        try:
            import ctranslate2
            return ctranslate2.get_cuda_device_count() > 0
        except Exception:  # noqa: BLE001
            return False
