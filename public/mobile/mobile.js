// DAW LIVE CAM — phone camera client.
// Streams a low-latency preview over WebRTC and records the full-quality
// video locally, uploading it in ordered, acknowledged chunks so a Wi-Fi
// hiccup never costs footage.

const $ = (id) => document.getElementById(id);
const KIND_CAMERA = 4;
const MIMES = ['video/mp4;codecs=avc1.640028', 'video/mp4;codecs=avc1', 'video/mp4', 'video/webm;codecs=h264', 'video/webm;codecs=vp9', 'video/webm;codecs=vp8', 'video/webm'];

const params = new URLSearchParams(location.search);
if (params.get('t')) { try { localStorage.setItem('dlcToken', params.get('t')); } catch { /* private mode */ } }
let token = params.get('t');
if (!token) { try { token = localStorage.getItem('dlcToken'); } catch { /* private mode */ } }

const want = { facing: 'environment', height: 1080, fps: 30 };
let stream = null, track = null, ws = null, pc = null, sender = null;
let desktopUp = false, retry = 500, wakeLock = null, battery = null, pendingIce = [];
let clock = { offset: null, rtt: null, samples: [] };
let R = null; // active recording
let cal = null;

// ───────────────────────────────────────────── helpers
const send = (msg) => { if (ws && ws.readyState === 1) ws.send(JSON.stringify(msg)); };
const toClock = (perf) => perf + clock.offset;
const fmt = (sec) => `${String(Math.floor(sec / 60)).padStart(2, '0')}:${String(Math.floor(sec % 60)).padStart(2, '0')}`;

function gate(title, text, button) {
  $('gate').hidden = false;
  $('gateTitle').textContent = title;
  $('gateText').textContent = text;
  $('startBtn').hidden = !button;
  if (button) $('startBtn').textContent = button;
}

function caps() {
  const c = track && track.getCapabilities ? track.getCapabilities() : {};
  const range = (r) => (r && typeof r.max === 'number' && r.max > r.min ? { min: r.min, max: r.max, step: r.step || 0.1 } : null);
  return {
    zoom: range(c.zoom), exposure: range(c.exposureCompensation),
    focus: c.focusMode && c.focusMode.includes('manual') ? range(c.focusDistance) : null,
    torch: !!c.torch, poi: !!c.pointsOfInterest || (c.focusMode || []).includes('single-shot'),
  };
}

function sendState() {
  if (!track) return;
  const s = track.getSettings();
  const c = caps();
  send({
    type: 'cam.state', facing: s.facingMode || want.facing, width: s.width, height: s.height, fps: s.frameRate,
    requestedHeight: want.height, caps: c, zoom: s.zoom, exposure: s.exposureCompensation,
    focusMode: s.focusMode, torch: !!s.torch, rtt: clock.rtt, hidden: document.hidden,
    battery: battery ? { level: battery.level, charging: battery.charging } : null, recording: !!R,
  });
  renderUi(s, c);
}

function renderUi(s, c) {
  $('fmt').textContent = s.width ? `${Math.min(s.width, s.height)}p · ${Math.round(s.frameRate || 0)} fps` : '—';
  $('cam').classList.toggle('mirror', (s.facingMode || want.facing) === 'user');
  const slider = (row, input, out, cap, value, f) => {
    row.hidden = !cap;
    if (!cap) return;
    input.min = cap.min; input.max = cap.max; input.step = cap.step;
    if (document.activeElement !== input && value != null) input.value = value;
    out.textContent = f(Number(input.value));
  };
  slider($('zoomRow'), $('zoom'), $('zoomOut'), c.zoom, s.zoom, (v) => `${v.toFixed(1)}×`);
  slider($('expRow'), $('exp'), $('expOut'), c.exposure, s.exposureCompensation, (v) => `${v > 0 ? '+' : ''}${v.toFixed(1)}`);
  $('torch').hidden = !c.torch;
  $('torch').classList.toggle('active', !!s.torch);
  $('flip').disabled = !!R;
  if (battery) {
    $('batt').hidden = false;
    $('batt').textContent = `${Math.round(battery.level * 100)}%${battery.charging ? ' ⚡' : ''}`;
    $('batt').classList.toggle('low', battery.level < 0.15 && !battery.charging);
  }
}

function renderLink() {
  const up = ws && ws.readyState === 1;
  $('link').className = `chip ${up && desktopUp ? 'on' : ''}`;
  $('link').lastElementChild.textContent = up && desktopUp
    ? `Connected${clock.rtt != null ? ` · ${Math.round(clock.rtt)} ms` : ''}`
    : R ? 'Reconnecting — still recording' : up ? 'Waiting for desktop app…' : 'Reconnecting…';
}

// ───────────────────────────────────────────── camera
async function startCamera() {
  if (R) return;
  if (stream) stream.getTracks().forEach((t) => t.stop()); // most phones allow one open camera
  const h = want.height;
  stream = await navigator.mediaDevices.getUserMedia({
    audio: false,
    video: { facingMode: { ideal: want.facing }, width: { ideal: Math.round((h * 16) / 9) }, height: { ideal: h }, frameRate: { ideal: want.fps } },
  });
  track = stream.getVideoTracks()[0];
  track.contentHint = 'motion';
  track.addEventListener('ended', () => { $('note').textContent = 'Camera stopped — tap Flip to restart.'; });
  $('cam').srcObject = stream;
  $('note').textContent = '';
  if (pc && sender && pc.connectionState !== 'closed' && pc.connectionState !== 'failed') {
    await sender.replaceTrack(track);
    tunePreview();
  } else if (desktopUp) await offer();
  sendState();
}

async function applyCam(patch) {
  if (!track) return;
  try { await track.applyConstraints({ advanced: [patch] }); } catch (e) { console.warn('constraint rejected', patch, e); }
  sendState();
}

async function onCamSet(m) {
  try {
    if (m.facing || m.height || m.fps) {
      if (R) return;
      const before = { ...want };
      if (m.facing) want.facing = m.facing;
      if (m.height) want.height = m.height;
      if (m.fps) want.fps = m.fps;
      try { await startCamera(); } catch (e) { Object.assign(want, before); await startCamera(); throw e; }
    }
    if (m.zoom != null) await applyCam({ zoom: m.zoom });
    if (m.exposure != null) await applyCam({ exposureMode: 'continuous', exposureCompensation: m.exposure });
    if (m.focusMode) await applyCam({ focusMode: m.focusMode });
    if (m.focusDistance != null) await applyCam({ focusMode: 'manual', focusDistance: m.focusDistance });
    if (m.torch != null) await applyCam({ torch: !!m.torch });
  } catch (e) {
    send({ type: 'cam.error', message: `Camera setting not available (${e.message})` });
  }
}

// ───────────────────────────────────────────── WebRTC preview
function tunePreview() {
  if (!sender || !track) return;
  const p = sender.getParameters();
  if (!p.encodings || !p.encodings.length) return;
  const s = track.getSettings();
  // The preview is only for monitoring: keep it light so the recorder gets the encoder.
  p.encodings[0].maxBitrate = 2_500_000;
  p.encodings[0].scaleResolutionDownBy = Math.max(1, Math.min(s.width || 720, s.height || 720) / 720);
  sender.setParameters(p).catch(() => {});
}

async function offer() {
  if (!track) return;
  if (pc) pc.close();
  pendingIce = [];
  const conn = new RTCPeerConnection({ iceServers: [] });
  pc = conn;
  sender = conn.addTrack(track, stream);
  conn.onicecandidate = (e) => { if (e.candidate) send({ type: 'rtc.ice', candidate: e.candidate }); };
  conn.onconnectionstatechange = () => {
    if (conn === pc && conn.connectionState === 'failed' && desktopUp) setTimeout(() => { if (conn === pc) offer(); }, 1000);
  };
  await conn.setLocalDescription(await conn.createOffer());
  tunePreview();
  send({ type: 'rtc.offer', sdp: conn.localDescription });
}

// ───────────────────────────────────────────── recording
function recStart(sessionId) {
  if (R || !stream) return;
  const mime = MIMES.find((m) => window.MediaRecorder && MediaRecorder.isTypeSupported(m));
  if (!mime) return send({ type: 'cam.error', message: 'This browser cannot record video. Update the phone browser.' });
  const s = track.getSettings();
  const short = Math.min(s.width || 1080, s.height || 1080);
  const bitrate = (short >= 2000 ? 45e6 : short >= 1000 ? 16e6 : 8e6) * ((s.frameRate || 30) > 40 ? 1.5 : 1);
  const mr = new MediaRecorder(stream, { mimeType: mime, videoBitsPerSecond: bitrate });
  const rec = { sessionId, mr, mime, queue: [], seq: 0, sent: 0, acked: 0, stopped: false, startPerf: null, paused: false, pausedTotal: 0, pausedAt: 0, busy: false };
  R = rec;
  send({ type: 'cam.begin', sessionId, mime });
  mr.onstart = () => {
    rec.startPerf = performance.now();
    const announce = () => {
      if (R !== rec) return;
      if (clock.offset == null) return setTimeout(announce, 100);
      send({ type: 'cam.rec.started', sessionId, startMs: toClock(rec.startPerf), mime, width: s.width, height: s.height, fps: s.frameRate });
    };
    announce();
  };
  mr.ondataavailable = (e) => { if (e.data && e.data.size) { rec.queue.push({ seq: ++rec.seq, blob: e.data }); pump(); } };
  mr.onstop = () => { rec.stopped = true; pump(); };
  mr.onerror = (e) => send({ type: 'cam.error', message: `Recorder error: ${e.error ? e.error.message : 'unknown'}` });
  mr.start(1000);
  document.body.classList.add('recording');
  $('rec').hidden = false;
  sendState();
}

function recStop() {
  if (R && R.mr.state !== 'inactive') R.mr.stop();
}

function recEnd() {
  R = null;
  document.body.classList.remove('recording');
  $('rec').hidden = true;
  $('rec').classList.remove('paused');
  sendState();
}

/** Upload queued chunks strictly in order; the server acknowledges each one. */
async function pump() {
  const rec = R;
  if (!rec || rec.busy) return;
  rec.busy = true;
  try {
    while (R === rec && ws && ws.readyState === 1 && ws.bufferedAmount < 8 << 20) {
      const next = rec.queue.find((c) => c.seq === rec.sent + 1);
      if (!next) break;
      const data = new Uint8Array(await next.blob.arrayBuffer());
      if (!ws || ws.readyState !== 1 || R !== rec || rec.sent + 1 !== next.seq) break;
      const out = new Uint8Array(8 + data.length);
      const dv = new DataView(out.buffer);
      dv.setUint32(0, KIND_CAMERA, true);
      dv.setUint32(4, next.seq, true);
      out.set(data, 8);
      ws.send(out);
      rec.sent = next.seq;
    }
  } finally {
    rec.busy = false;
  }
  if (R === rec && rec.stopped && rec.acked === rec.seq) {
    send({ type: 'cam.done', sessionId: rec.sessionId, chunks: rec.seq });
    recEnd();
  }
}

function onAck(seq) {
  if (!R) return;
  R.acked = Math.max(R.acked, seq);
  R.queue = R.queue.filter((c) => c.seq > R.acked);
  if (R.sent < R.acked) R.sent = R.acked;
  pump();
}

// ───────────────────────────────────────────── sync calibration (flash detection)
function calStart() {
  const v = $('cam');
  const cv = document.createElement('canvas');
  cv.width = cv.height = 24;
  const ctx = cv.getContext('2d', { willReadFrequently: true });
  const run = { base: null, last: 0, on: true };
  cal = run;
  const step = () => {
    if (!run.on) return;
    ctx.drawImage(v, 0, 0, 24, 24);
    const d = ctx.getImageData(0, 0, 24, 24).data;
    let sum = 0;
    for (let i = 0; i < d.length; i += 4) sum += d[i] * 0.299 + d[i + 1] * 0.587 + d[i + 2] * 0.114;
    const luma = sum / 576;
    const now = performance.now();
    if (run.base == null) run.base = luma;
    if (luma - run.base > 22 && now - run.last > 400 && clock.offset != null) {
      run.last = now;
      send({ type: 'cal.edge', t: toClock(now) });
    }
    // Slow baseline so the flash itself stands out, but exposure changes are followed.
    run.base += (luma - run.base) * (luma > run.base ? 0.02 : 0.2);
    if (v.requestVideoFrameCallback) v.requestVideoFrameCallback(step); else requestAnimationFrame(step);
  };
  step();
}

// ───────────────────────────────────────────── socket
function onMessage(m) {
  switch (m.type) {
    case 'peer':
      desktopUp = m.connected;
      if (R) {
        if (m.recording === R.sessionId) {
          // Resume the upload exactly where the server left off.
          R.sent = R.acked = m.camSeq || 0;
          R.queue = R.queue.filter((c) => c.seq > R.acked);
          send({ type: 'cam.begin', sessionId: R.sessionId, mime: R.mime });
          pump();
        } else if ('recording' in m) {
          // The session ended while we were away.
          R.mr.onstop = null; R.mr.ondataavailable = null;
          if (R.mr.state !== 'inactive') R.mr.stop();
          recEnd();
        }
      }
      renderLink();
      break;
    case 'hello':
      desktopUp = true; renderLink();
      for (const d of [0, 150, 400, 900]) setTimeout(() => send({ type: 'ping', t0: performance.now() }), d);
      offer().catch(() => {}); sendState();
      break;
    case 'rtc.answer':
      if (pc) pc.setRemoteDescription(m.sdp).then(() => { for (const c of pendingIce.splice(0)) pc.addIceCandidate(c).catch(() => {}); }).catch(() => {});
      break;
    case 'rtc.ice':
      if (pc && pc.remoteDescription) pc.addIceCandidate(m.candidate).catch(() => {}); else pendingIce.push(m.candidate);
      break;
    case 'pong': {
      const t1 = performance.now();
      clock.samples.push({ rtt: t1 - m.t0, offset: m.td - (m.t0 + t1) / 2 });
      if (clock.samples.length > 20) clock.samples.shift();
      // The fastest round trip is the least distorted by network jitter.
      const best = clock.samples.reduce((a, b) => (b.rtt < a.rtt ? b : a));
      clock.offset = best.offset;
      clock.rtt = t1 - m.t0;
      renderLink();
      break;
    }
    case 'cam.set': onCamSet(m); break;
    case 'cam.rec.start': recStart(m.sessionId); break;
    case 'cam.rec.stop': recStop(); break;
    case 'cam.rec.pause':
      if (R) {
        const now = performance.now();
        if (m.paused && !R.paused) R.pausedAt = now;
        if (!m.paused && R.paused) R.pausedTotal += now - R.pausedAt;
        R.paused = m.paused;
        $('rec').classList.toggle('paused', R.paused);
      }
      break;
    case 'cam.ack': onAck(m.seq); break;
    case 'cal.start': calStart(); break;
    case 'cal.stop': if (cal) cal.on = false; cal = null; break;
  }
}

function connect() {
  const sock = new WebSocket(`wss://${location.host}/ws?t=${encodeURIComponent(token)}`);
  sock.binaryType = 'arraybuffer';
  ws = sock;
  sock.onopen = () => { retry = 500; clock.samples = []; renderLink(); send({ type: 'ping', t0: performance.now() }); };
  sock.onmessage = (e) => { if (typeof e.data === 'string') onMessage(JSON.parse(e.data)); };
  sock.onclose = (e) => {
    if (ws !== sock) return;
    ws = null; desktopUp = false;
    if (pc) { pc.close(); pc = null; sender = null; }
    renderLink();
    if (e.code === 4001) return gate('Pairing was reset', 'Scan the new code shown in the desktop app to connect this phone again.');
    if (e.code === 4000) return gate('Opened elsewhere', 'This camera was replaced by another phone or tab.', 'Use this phone');
    setTimeout(connect, retry);
    retry = Math.min(3000, retry * 1.6);
  };
  sock.onerror = () => {};
}

// ───────────────────────────────────────────── boot
async function keepAwake() {
  try { if ('wakeLock' in navigator && !wakeLock && !document.hidden) { wakeLock = await navigator.wakeLock.request('screen'); wakeLock.addEventListener('release', () => { wakeLock = null; }); } } catch { /* denied on low battery */ }
}

async function begin() {
  try {
    await startCamera();
  } catch (e) {
    const denied = e.name === 'NotAllowedError';
    return gate(denied ? 'Camera blocked' : 'Camera unavailable',
      denied ? 'Allow camera access for this page in the browser settings, then try again.' : e.message, 'Try again');
  }
  $('gate').hidden = true;
  $('controls').hidden = false;
  keepAwake();
  if (!('wakeLock' in navigator)) $('note').textContent = 'Set Auto-Lock to “Never” so the screen stays on.';
  if (navigator.getBattery) {
    try {
      battery = await navigator.getBattery();
      battery.addEventListener('levelchange', sendState);
      battery.addEventListener('chargingchange', sendState);
    } catch { /* unsupported */ }
  }
  if (!ws) connect();
  sendState();
}

$('startBtn').onclick = begin;
$('flip').onclick = () => onCamSet({ facing: want.facing === 'user' ? 'environment' : 'user' });
$('torch').onclick = () => applyCam({ torch: !track.getSettings().torch });
$('zoom').oninput = () => { $('zoomOut').textContent = `${Number($('zoom').value).toFixed(1)}×`; applyCam({ zoom: Number($('zoom').value) }); };
$('exp').oninput = () => applyCam({ exposureMode: 'continuous', exposureCompensation: Number($('exp').value) });
$('cam').addEventListener('pointerdown', (e) => {
  if (!track || !caps().poi) return;
  const r = $('cam').getBoundingClientRect();
  const ring = $('focusRing');
  ring.style.left = `${e.clientX}px`; ring.style.top = `${e.clientY}px`;
  ring.classList.remove('show'); void ring.offsetWidth; ring.classList.add('show');
  applyCam({ pointsOfInterest: [{ x: (e.clientX - r.left) / r.width, y: (e.clientY - r.top) / r.height }], focusMode: 'single-shot' })
    .then(() => setTimeout(() => applyCam({ focusMode: 'continuous' }), 1500));
});
$('cam').addEventListener('resize', sendState);
document.addEventListener('visibilitychange', () => { keepAwake(); sendState(); });

setInterval(() => { if (ws && ws.readyState === 1 && desktopUp) send({ type: 'ping', t0: performance.now() }); }, 2000);
setInterval(() => {
  sendState();
  if (R) {
    pump();
    if (R && R.startPerf != null && clock.offset != null && !R.stopped) {
      const now = performance.now();
      send({ type: 'cam.marker', sessionId: R.sessionId, mediaMs: now - R.startPerf, clockMs: toClock(now) });
    }
  }
}, 5000);
setInterval(() => {
  if (R && R.startPerf != null) {
    const now = performance.now();
    const paused = R.pausedTotal + (R.paused ? now - R.pausedAt : 0);
    $('recTime').textContent = fmt((now - R.startPerf - paused) / 1000);
  }
  if (R) pump();
}, 500);

if (!token) {
  gate('Scan to pair', 'Open DAW LIVE CAM on your computer, choose “Connect phone” and scan the code with this phone.');
} else if (!window.isSecureContext || !navigator.mediaDevices) {
  gate('Secure connection needed', 'Open the https:// link from the desktop app’s pairing code so the camera can be used.');
} else if (navigator.permissions && navigator.permissions.query) {
  // Returning phones go straight to the live camera.
  navigator.permissions.query({ name: 'camera' }).then((p) => { if (p.state === 'granted') begin(); }).catch(() => {});
}
