# AutoProducer

AutoProducer composes, arranges, mixes and exports a complete song inside Ableton Live 12. It works over AbletonOSC and uses only two sources: your own sample library and your whitelisted VST3 plugins.

```
step0 → index → init → plan → sketch ─► CHECKPOINT A ─► produce ─► CHECKPOINT B ─► deliver
                                         (approve A)    arrange, mix, master,      (approve B)
                                                        render + QC loop (≤5)
```

## Install (on the machine running Live)

```bash
cd autoproducer
python -m venv .venv && . .venv/bin/activate      # Windows: .venv\Scripts\activate
pip install -e ".[gui,dev]"
cp config.example.yaml config.yaml                 # fill in every TODO
```

1. **AbletonOSC:** put the [AbletonOSC](https://github.com/ideoforms/AbletonOSC) folder in your Remote Scripts folder, then select it under Preferences → Link, Tempo & MIDI → Control Surface.
2. **Extension:** run `autoproducer install-ext --abletonosc "<Remote Scripts>/AbletonOSC"`, then restart Live.
   It copies one handler file and adds two lines, writing `.bak` backups first. Running it again changes nothing.
3. **One-time export setup:** open File → Export Audio/Video once and set:
   - Rendered Track: **All Individual Tracks**
   - WAV, **24-bit**, **48000 Hz**
   - Normalize **off**
   - Create Analysis File **off**

   Live remembers these settings, and the pipeline reuses them.
4. **Permissions:**
   - **macOS:** give your terminal Accessibility permission (System Settings → Privacy & Security). The Save As and Export steps send keystrokes.
   - **Windows:** nothing extra is needed.

## Run

```bash
autoproducer step0          # readiness table. Fix every ❌, then re-run
autoproducer index          # scans the library (read-only); cached in <project_path>/.autoproducer_index.sqlite
# In Live: File → New Live Set
autoproducer init           # saves the empty set as "<song> Project/<song>.als"
autoproducer plan           # key, BPM, progression, sections, energy, sample shortlist → plan.md
autoproducer sketch         # tracks, verified presets, 8-bar loop under the loop brace, saves v001
autoproducer approve A      # after listening
autoproducer produce        # arrange → mix → master → render/QC, with a versioned save before each stage
autoproducer approve B      # copies master, stems, MP3 and report.md into Deliverables/
```

Do not touch the mouse or keyboard while a Save or Export step runs. To abort one, move the mouse into a screen corner (pyautogui failsafe).

## How each hard rule is enforced

| Rule | Mechanism |
|---|---|
| Own samples + whitelisted VST3 only | Every preset load is followed by a device readback (`name`, `class_name`). A non-whitelisted plugin is deleted and the run stops. `produce` audits all tracks before rendering, and `report.md` lists every plugin found in the saved `.als`. |
| No silent substitution | A role with no compatible sample is dropped from the plan and listed under "unfilled_roles". A missing plugin or preset is a ❌ in step0. |
| Non-destructive | The library is opened read-only. Samples are copied into `Samples/Imported/AutoProducer/` before use, a new project folder is required, and every stage gets a new `_v###.als`. The report proves self-containment by listing every file the `.als` references from outside the project. |
| Musical coherence | One key per song. Tonal loops are transposed (±2 st max) and warped Complex Pro; drum loops are warped Beats. Loop tempo is verified from the warp markers and corrected if it is more than 1 % off. |
| Two checkpoints | `produce` refuses to run until A is approved. Delivery happens only through `approve B`. |
| Never claim success unmeasured | QC numbers (pyloudnorm LUFS-I, 4× oversampled true peak, clipping, silence, L/R correlation, mono loss, side energy < 120 Hz, format, duration) go into `report.md` for every render iteration. |

Every OSC call, file copy, GUI step and QC result is appended to `autoproducer/actions.jsonl` in the project.

## Known limits and things to confirm on first real run

This code has been tested here against a stubbed Live object model, not against Live 12.4.3 itself. On the first run, check these points:

- **Arrangement MIDI clips:** created with `Track.create_midi_clip`. If your Live build lacks it, the code falls back to session slot → `duplicate_clip_to_arrangement`.
- **Warp fix:** the warp correction uses `Clip.add_warp_marker` / `remove_warp_marker`. If it errors, the run stops rather than leaving an off-tempo loop.
- **Parameter names:** stock-device parameter names (EQ Eight `1 Frequency A`, Limiter `Ceiling` / `Gain`, Utility `Bass Mono`) are matched loosely. Anything not found is logged and named in the report.
- **Sidechain:** realized as kick-synced volume ducking (a Utility gain envelope, −9 dB per kick). The API cannot reliably set compressor sidechain routing.
- **Groups:** the API cannot create group tracks, so stems are per track.
- **Collect All and Save:** not automated. Samples are copied in up front, and the `.als` audit verifies nothing points outside the project.
- **Drums (default `drums.mode: audio_hits`):** every hit is an unwarped one-shot clip of your own kick/snare/clap/hat/perc samples on its own track. Hits sit at their humanized position, and velocity is applied as clip gain (≤ 0 dB). Each hit is trimmed so it never overlaps the next. `plan` stops if the library has no kick, hat, or snare/clap. Hits are sent 150 per OSC message. A drum preset is not needed (`drums.mode: midi` + `presets.drums` is the alternative).
  - To confirm on first run: clip end markers on unwarped clips. The extension tries seconds, then beats, verifies the result, and counts failures in the action log as `trim_failed`.
- **Export file naming:** Live's export file names are matched by "* Master.wav". If the export dialog fails, the pipeline falls back to resampling, which records the master only and no stems.

## Tests

```bash
pytest -q    # music rules, QC maths, index, OSC bridge with the real extension against a fake Live, installer
```
