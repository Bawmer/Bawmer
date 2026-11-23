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
        try:
            model = WhisperModel(model_name, device=device, compute_type=compute, download_root=download_root)
        except Exception as e:  # noqa: BLE001 - missing CUDA/cuDNN runtime etc.
            if device == "cpu":
                raise
            P.log(f"GPU transcription unavailable ({e}); using CPU")
            device, compute = "cpu", "int8"
            model = WhisperModel(model_name, device=device, compute_type=compute, download_root=download_root)

        def run(vad: bool):
            # Singing-friendly decoding: no conditioning on previous text (prevents the
            # repetition loops Whisper falls into on sustained notes), generous VAD padding
            # so held vowels are not cut, optional lyrics hint as the initial prompt.
            segments, info = model.transcribe(
                args.audio, language=language, word_timestamps=True, beam_size=5,
                condition_on_previous_text=False, initial_prompt=(args.prompt or None),
                vad_filter=vad, vad_parameters=dict(min_silence_duration_ms=700, speech_pad_ms=400) if vad else None,
                no_speech_threshold=0.5, compression_ratio_threshold=2.6)
            words, text = [], []
            duration = max(1e-6, float(getattr(info, "duration", 0.0) or 1.0))
            for seg in segments:
                text.append(seg.text.strip())
                for w in seg.words or []:
                    words.append({"word": w.word.strip(), "start": float(w.start), "end": float(w.end),
                                  "probability": float(getattr(w, "probability", 1.0))})
                P.progress(0.1 + 0.85 * min(1.0, seg.end / duration), f"{seg.end:.0f}/{duration:.0f} s")
            return info, words, text

        try:
            info, words, text = run(vad=True)
        except RuntimeError as e:          # CUDA libraries load lazily: fall back on first use too
            if device == "cpu":
                raise
            P.log(f"GPU transcription failed ({e}); using CPU")
            device, compute = "cpu", "int8"
            model = WhisperModel(model_name, device=device, compute_type=compute, download_root=download_root)
            info, words, text = run(vad=True)
        if not words:   # the speech VAD can reject singing - retry on the whole file
            P.log("No words with voice-activity detection, retrying without it")
            info, words, text = run(vad=False)
        words = [w for w in words if w["word"] and w["end"] >= w["start"]]
        return {"language": info.language, "text": " ".join(t for t in text if t), "words": words,
                "model": model_name}

    try:
        import whisper  # openai-whisper
    except ImportError as e:
        raise RuntimeError("No speech recogniser installed. Run:  pip install faster-whisper") from e

    P.progress(0.05, f"Loading openai-whisper '{model_name}'", force=True)
    model = whisper.load_model(model_name, download_root=download_root)
    res = model.transcribe(args.audio, language=language, word_timestamps=True,
                           condition_on_previous_text=False, initial_prompt=(args.prompt or None))
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
