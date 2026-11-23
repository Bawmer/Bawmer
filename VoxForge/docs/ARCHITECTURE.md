# VoxForge architecture

## Overview

```text
 DAW ── MIDI ──► VoxForgeProcessor ──────────────────────────────► audio out (44.1–192 kHz)
                  │  capture FIFO (audio → message thread)            ▲
                  │  live voice (real-time formant synth) ────────────┤
                  │  rendered take playback (host ppq → project time) ┘
                  │
     editor ◄─────┤ VocalProject (notes in beats, lyrics, CC curves, stem)
                  │
                  ├─► RenderEngine thread (offline, 48 kHz)
                  │      Timeline (phonemes) + ControlCurves (f0, dynamics, breath, tension)
                  │      ├─ DSP voice:    SynthRenderer → spectral fingerprint (Lite clone)
                  │      └─ Neural voice: SynthRenderer guide → content_encoder.onnx → voice.onnx
                  │         Replacement: stem → units (time-warped to edited notes) + edited f0 → voice.onnx
                  │      → resample to host rate → cached WAV
                  │
                  ├─► StemAnalyzer thread: YIN f0 → note segmentation → (engine) Whisper words → notes
                  │
                  └─► EngineJob (child process: voxforge-engine train …) → voice.onnx
```

## Threads

| Thread | Work | Shares data through |
|---|---|---|
| Audio | MIDI capture, live voice, playback of the take and the stem | `AbstractFifo` of capture events; take and stem behind `SpinLock` with try-lock (never blocks); replaced takes go to a graveyard that is freed on the message thread |
| Message | Editor, project edits, draining the capture FIFO, debounced auto-render, installing takes | Owns `VocalProject` |
| Render | Synthesis, ONNX inference, resampling, cache | Gets a full copy of the project (`RenderRequest`); a new request cancels the running one |
| Stem analysis | Vocal-to-MIDI, transcription | Result polled by the editor |
| Engine job | Neural training in a separate process | JSON lines on stdout |

## Singing synthesis (DSP engine)

1. **Lyrics → phonemes.** `Lexicon` looks words up in three places in order: the user's CMU
   dictionary, a built-in singing dictionary, then compact English letter-to-sound rules.
   `syllabifyPhonemes` splits words with one vowel nucleus per syllable and the maximal legal
   onset. `applyLyricsToNotes` places one syllable per note; `-` marks a melisma.
2. **Timeline.** Notes and their following holds are grouped into syllables. Onset consonants
   are placed *before* the note so the vowel starts on the beat; their duration is borrowed
   from the gap or from the previous vowel. Coda consonants close the syllable and take at most
   40 % of the vowel.
3. **Control curves** run at 1500 Hz. The pitch is the sum of logistic transitions between
   notes (portamento for legato, fast jumps hidden in consonants), plus vibrato (delay, fade-in,
   per-note rate variation, CC1), plus drift and phrase-start scoops (Humanize), plus pitch bend.
   Dynamics come from velocity, Expression and CC11, with attack, release and a slight swell.
   Breath comes from the knob and CC2; tension from the knob and velocity.
4. **VoiceDSP** is a Klatt-style cascade of five formant resonators plus a nasal pole/zero pair,
   with a parallel frication band-pass filter and a "singer's formant" peak. The source is a
   KLGLOTT88 glottal-flow derivative with polyBLEP anti-aliasing at glottal closure. Tension
   sets the open quotient and spectral tilt. Aspiration noise is pulsed with the glottal cycle,
   and jitter and shimmer are added. F1 follows f0 on high notes (formant tuning). Targets are
   smoothed at control rate: 14 ms for formants, 4 ms for amplitudes. The class does not depend
   on sample rate, so the same DSP runs offline at 48 kHz and live at any host rate.

### Lite clone

`analyseVoiceSamples` works in these steps:

1. Pitch-track the user's samples and record their pitch statistics.
2. Find vibrato rate and depth from the autocorrelation of the detrended pitch curve.
3. Measure breathiness from the spectral flatness between 2 and 6 kHz.
4. Build a **harmonic-peak spectral envelope**: the spectrum is sampled at each harmonic and
   interpolated in log-frequency. This makes the envelope independent of pitch.
5. Render a reference phrase with the synth and search for the vocal-tract scale that best
   correlates the two envelopes between 250 and 4500 Hz. Tests show it recovers the true value
   to within 0.02.
6. Store the remaining difference as a smoothed 40-band correction curve. At render time it is
   applied with an STFT (Hann window, 75 % overlap, perfect reconstruction at 0 dB).

## Neural voice

This is singing voice conversion with frame-level conditioning, so no duration model or forced
alignment is needed.

| Model | Inputs | Output |
|---|---|---|
| `content_encoder.onnx` (shared) | `audio` `[1, N]` float32 @ 16 kHz | `units` `[1, T, 768]`: HuBERT-Base layer 12, 50 frames/s |
| `voice.onnx` (per voice) | `units` `[1, T, 768]`, `f0` `[1, T]` (Hz, 0 = unvoiced) | `audio` `[1, T·960]` @ 48 kHz |

`voice.onnx` is **RVC v2 48 kHz** (`SynthesizerTrnMs768NSFsid`, model code vendored from rvc-python,
MIT) wrapped to the plug-in contract: units are repeated ×2 to 100 frames/s, f0 is quantised to RVC's
256 mel-pitch bins, speaker 0 is used, and the prior is sampled with σ = 0.667. The graph is: text/content
encoder (6-layer relative-attention transformer over units + pitch embedding) → normalising flow (reverse)
→ NSF-HiFiGAN decoder (×12 ×10 ×2 ×2, sine excitation from f0) → 48 kHz audio.

Training fine-tunes the official pretrained RVC v2 48 kHz base (`f0G48k.pth` / `f0D48k.pth`, downloaded
automatically) with the VITS objective: multi-period discriminator V2, mel L1 ×45, KL, feature matching,
LSGAN; AdamW 1e-4, lr decay 0.999875/epoch, 0.36 s slices from 0.96 s crops, ±2 st pitch augmentation.
Phrases are cut at silences and cached with HuBERT layer-12 units (50 fps), WORLD f0 (100 fps) and
48 kHz audio. Training resumes from `checkpoints/latest.pt`.

### Rendering with a neural voice

- **From MIDI.** The DSP engine renders a neutral guide vocal that carries the timing and
  articulation. The content encoder turns it into units, and `voice.onnx` re-synthesises them
  with the exact target f0 from the control curves. Unvoiced frames come from the phoneme
  timeline. Inference runs in chunks of 8 s with 0.48 s context and a 160 ms crossfade.
- **Replacement.** The stem's units are computed once and cached by path and modification time.
  A monotonic piecewise-linear warp maps each output frame to stem time; anchors are the
  start and end of every source note whose word is unchanged. Notes with new words and added
  notes take their units from a synthesised guide, crossfaded over ±3 frames in unit space.
  f0 is the edited melody plus *nuance* × the original deviation from the note centre;
  voicing comes from the stem. A volume-follow stage restores the original dynamics in
  source regions.

## Vocal-to-MIDI

The pitch tracker is FFT-accelerated YIN at 16 kHz with a 10 ms hop. It adds parabolic
refinement, a sub-octave check (strong second harmonics, where F1 ≈ 2·f0, otherwise cause
octave-up errors), energy gates, 5-point median and octave repair, and removal of short blips.

Segmentation works in these steps:

1. Bridge unvoiced gaps of up to 30 ms.
2. Within each voiced run, start a new note on a sustained deviation from the running median
   pitch (more than *Split* semitones for 60 ms or longer), or on an energy onset of +7 dB
   (repeated syllables on the same pitch).
3. Take each note's pitch as the median of its central 70 %.
4. Merge short fragments into their neighbours, then merge neighbours that round to the same
   pitch when there is no onset between them.
5. Fix a single note that sits an octave away from both neighbours.

Lyrics come from faster-whisper word timestamps. Each word's syllables are spread over the
notes whose centres fall inside the word. Notes between words keep `•`, which means "original
sound".

## State and files

The plug-in state is XML in the host project. It holds:

- the parameters (APVTS)
- `VocalProject`: notes with source mapping, lyrics, CC curves as base64, stem path and offset
- the voice id
- the take hash

The rendered take is cached in `Cache/<hash>.wav`, where the hash covers notes, curves,
expression settings, the voice and its model timestamp. Reopening a project loads the cache
or re-renders.

A voice preset is the folder `Voices/<name>/` containing `voice.json`
(`format: voxforge-voice`, timbre, expression defaults, stats, the neural model reference and
the sample list), `samples/`, and an optional `voice.onnx`. A `.vfvoice` file is a zip of that
folder.
