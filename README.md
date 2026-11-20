# DAW LIVE CAM

Record your whole production session: your phone is the wireless camera, and the
audio comes straight from your DAW as a WAV file. Picture and sound are aligned
automatically and exported as MP4 for YouTube, Reels, TikTok or Shorts.

**CONNECT PHONE → CONNECT DAW → PREVIEW → RECORD → EXPORT**

## Download

Windows x64: [`release/DAW-LIVE-CAM-win-x64.exe`](release/DAW-LIVE-CAM-win-x64.exe) — portable, double-click to run (unsigned: SmartScreen → *More info* → *Run anyway*).

## Run it

```bash
npm install
npm start          # desktop app (Electron, Windows + macOS)
npm run serve      # same engine without Electron; open the printed URL in Chrome/Edge
npm run dist       # build installers (electron-builder)
```

1. **Camera:** choose **Phone (Wi-Fi)**, **USB camera** or **Phone + USB** in the
   Camera card. *Phone + USB* uses both at once — e.g. the webcam on your face
   and the phone on your hands — in the preview, the recording, the export and
   the live stream (two boxes over the DAW, the camera side split in two, or
   both cameras next to each other under the DAW on vertical streams).
   For a USB webcam just pick it from the list — nothing else to set up.
   For the phone, click **Connect phone** and scan the QR code (same Wi-Fi).
   The phone page uses a private self-signed certificate, so accept the browser
   warning once, then tap **Start camera**. Add it to the home screen to launch
   it like an app; it reconnects by itself afterwards.
2. Choose the audio **Source**, then **Connect DAW audio** (see *Routing help* in the app).
3. Leave **Capture the DAW screen** ticked (Studio Session). In the Windows app a
   picker opens: choose your **DAW window** (or a whole screen). The preview then
   shows the final picture as two screens side by side: your complete DAW and
   you talking (default: you in a square box over the full DAW — switch *Picture* to *Side by side* for two panels). Drag the line between them (or use *Camera size*) to resize,
   use *Swap DAW and camera* to change sides, or switch *Picture* to
   *Square box* for a small square camera over the full DAW (S / M / L, drag to
   any corner).
4. Press **Record**. The dashboard minimises itself so the full-screen DAW is
   captured cleanly; a small floating bar (left out of the recording) shows the
   time with Pause / Stop / Show buttons. Shortcuts from inside the DAW:
   **Ctrl+Alt+R** record/stop, **Ctrl+Alt+P** pause, **Ctrl+Alt+D** dashboard.
   Untick *Minimise this window while recording or live* to keep it open.
   The window is out of the way *before* the first frame is recorded.
   **Change the captured screen or window at any time** — while recording or
   live — with *Change* (DAW Audio card), *Change screen* (Live card) or the 🖥
   button on the floating bar; recording and stream keep running without a gap.
5. The session opens in **Sessions**: preview, trim, fine-tune sync, export.

## Instagram 16:9 split: phone 2/3 + DAW timeline 1/3

Click **⧉ Instagram 16:9 · phone 2/3 + DAW timeline** (Camera card, once the
DAW screen is captured). It sets side by side with the phone camera in the
left 2/3, switches the live stream to Instagram 16:9, and opens the **DAW part**
picker: drag the frame over your timeline (drag the corner dot to resize).
The frame has the shape of the DAW panel, so the timeline fills it exactly.
The full screen is still recorded — change the part any time with
*Choose DAW part…* in the studio or the editor, or go back to *Whole DAW*.
The camera can take 20–80 % of the width (slider or drag the divider).

## Live on Instagram or TikTok

The **Live stream** card sends the same picture (DAW + you) with the DAW audio.

- **Instagram:** on instagram.com (computer) → Create → Live video → copy the
  *Stream URL* and *Stream key*. Press **Go live** here, then Go live on
  Instagram once its preview shows you. Keys are new for every broadcast.
- **TikTok:** needs LIVE access with a stream key (LIVE Center / LIVE Studio →
  Stream key). Copy the *Server URL* and *Stream key*. Not every account gets one.
- **Other:** any RTMP/RTMPS server (YouTube, Twitch, Facebook, Restream…).

Vertical 9:16 puts the DAW across the top and you below (or the studio
picture, if chosen); 720p at 4 Mbps or 1080p at 6 Mbps, H.264 + AAC, 2 s
keyframes. A dropped connection is retried automatically (5 times). You can
record and stream at the same time. Stream keys are saved only on this computer.

## What it does

| Area | Details |
| --- | --- |
| Camera | Phone over Wi-Fi or a USB webcam on the computer (recorded locally, same clock as the audio). Front/rear, 720p / 1080p / 4K, 24 / 30 / 60 fps, zoom, exposure, manual focus, tap-to-focus, light, rotation — all remote-controlled from the desktop where the phone browser exposes them. Battery, link latency and format are shown live. Screen stays awake. |
| Video quality | The phone records the full-quality file itself and uploads it in ordered, acknowledged chunks. The WebRTC stream is only the low-latency preview, so Wi-Fi dips never degrade or lose footage. |
| Audio | Stereo WAV, 24-bit PCM or 32-bit float, 44.1 / 48 / 96 kHz, RF64 beyond 4 GB. Optional microphone on its own track. No processing (echo cancel, AGC, noise suppression all off). |
| Audio sources | Windows: system loopback (WASAPI) with one click. Any OS: virtual drivers (BlackHole, Loopback, VB-CABLE, VoiceMeeter) or an interface's loopback input. Works with Ableton Live, FL Studio, Logic Pro, Pro Tools, Cubase, Studio One, Reaper, Bitwig — anything that plays through an audio device. |
| Sync | Phone and desktop clocks are synchronised continuously; audio is stamped by sample count. Markers every 5 s measure clock drift, which is corrected on long takes by retiming the video (audio is never stretched). A flash/click calibration tool measures camera and audio latency; a ±500 ms manual adjustment is available in the editor. |
| Modes | Studio Session (camera + DAW screen), Performance (camera), Tutorial (screen + camera split), Social Media (9:16 with a framing guide). |
| Editor | Preview, trim, sync adjust, rotate; layouts: **side by side — complete DAW + camera** (default; swap sides; stacked for 9:16), DAW + small square camera (size and corner adjustable), camera, screen. The DAW is never cropped; export MP4 (H.264/AAC) in 16:9, 9:16, 1:1 at 720p–4K, and WAV in the original format. Edits are saved with the project. |
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
