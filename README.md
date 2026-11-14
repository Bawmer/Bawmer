# DAW LIVE CAM

Record your whole production session: your phone is the wireless camera, and the
audio comes straight from your DAW as a WAV file. Picture and sound are aligned
automatically and exported as MP4 for YouTube, Reels, TikTok or Shorts.

**CONNECT PHONE → CONNECT DAW → PREVIEW → RECORD → EXPORT**

## Run it

```bash
npm install
npm start          # desktop app (Electron, Windows + macOS)
npm run serve      # same engine without Electron; open the printed URL in Chrome/Edge
npm run dist       # build installers (electron-builder)
```

1. Click **Connect phone** and scan the QR code with the phone (same Wi-Fi).
   The phone page uses a private self-signed certificate, so accept the browser
   warning once, then tap **Start camera**. Add it to the home screen to launch
   it like an app; it reconnects by itself afterwards.
2. Choose the audio **Source**, then **Connect DAW audio** (see *Routing help* in the app).
3. Press **Record**. Pause/resume as needed, press again to stop.
4. The session opens in **Sessions**: preview, trim, fine-tune sync, export.

## What it does

| Area | Details |
| --- | --- |
| Camera | Front/rear, 720p / 1080p / 4K, 24 / 30 / 60 fps, zoom, exposure, manual focus, tap-to-focus, light, rotation — all remote-controlled from the desktop where the phone browser exposes them. Battery, link latency and format are shown live. Screen stays awake. |
| Video quality | The phone records the full-quality file itself and uploads it in ordered, acknowledged chunks. The WebRTC stream is only the low-latency preview, so Wi-Fi dips never degrade or lose footage. |
| Audio | Stereo WAV, 24-bit PCM or 32-bit float, 44.1 / 48 / 96 kHz, RF64 beyond 4 GB. Optional microphone on its own track. No processing (echo cancel, AGC, noise suppression all off). |
| Audio sources | Windows: system loopback (WASAPI) with one click. Any OS: virtual drivers (BlackHole, Loopback, VB-CABLE, VoiceMeeter) or an interface's loopback input. Works with Ableton Live, FL Studio, Logic Pro, Pro Tools, Cubase, Studio One, Reaper, Bitwig — anything that plays through an audio device. |
| Sync | Phone and desktop clocks are synchronised continuously; audio is stamped by sample count. Markers every 5 s measure clock drift, which is corrected on long takes by retiming the video (audio is never stretched). A flash/click calibration tool measures camera and audio latency; a ±500 ms manual adjustment is available in the editor. |
| Modes | Studio Session (camera + DAW screen), Performance (camera), Tutorial (screen + camera split), Social Media (9:16 with a framing guide). |
| Editor | Preview, trim, sync adjust, rotate; layouts: camera, screen, inset, split; export MP4 (H.264/AAC) in 16:9, 9:16, 1:1 at 720p–4K, and WAV in the original format. Edits are saved with the project. |
| Privacy | Everything stays on your machine and local network. No cloud, no accounts, no STUN/TURN servers. The desktop API only answers on `127.0.0.1`; the phone needs the pairing token. |

Recordings live in `Videos/DAW Live Cam/Sessions/<id>/` (`~/DAW Live Cam` with
`npm run serve`): `audio.wav`, `mic.wav`, `camera.mp4|webm`, `screen.webm`,
`project.json`, `exports/`. Originals are never modified. A pause is stored as a
cut range, so the originals stay continuous and in sync.

## Architecture

```
electron/main.js       window, permissions, Windows system-audio loopback
server/app.js          local HTTP + WebSocket hub, recording writer, exports
server/wav.js          streaming WAV / RF64 writer
server/sync.js         clock models, drift fit, offsets
server/exporter.js     ffmpeg filter graphs (bundled ffmpeg-static)
public/desktop/        studio UI, audio engine (AudioWorklet), editor
public/mobile/         phone camera app (installable web app)
```

## Tests

```bash
npm test               # unit: WAV writer, sync maths, export graph
npm run test:e2e       # full flow in real browsers with simulated devices (needs Playwright)
```

## Known limits

- **Pairing uses a QR code over Wi-Fi, not Bluetooth.** The phone side is an
  installable web app rather than App Store / Play Store binaries, and browsers
  cannot be discovered over Bluetooth LE.
- **macOS** has no built-in system-audio capture in Electron: use BlackHole or
  Loopback as described in *Routing help*.
- An **exclusive-mode ASIO** driver bypasses Windows system capture; use
  WASAPI or a virtual cable.
- Camera controls depend on the phone browser: Android Chrome exposes zoom,
  focus, exposure and light; iOS Safari exposes fewer, and no battery level.
- Resolution/frame rate are what the browser grants; 4K and 60 fps are not
  available on every phone.
- Rotating the phone mid-take changes the frame size; lock orientation first.
- If the phone is offline when recording stops, the desktop waits 20 s for the
  remaining footage before saving.

---

This repository also contains **Drumkit Clone**, a VST3 instrument that turns any loop, one-shot or song into a playable drum kit: see [`drumkit-clone/`](drumkit-clone/README.md).
