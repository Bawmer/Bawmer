import { $, toast, api, fmtClock, fmtBytes, dB, fitCanvas, segmented, setSeg, debounce, pipBox, splitBoxes, frameBoxes, placeVideo, placeScreen, validCrop, dragToCorner, pipFraction, toSlider, fromSlider, dragDivider } from './util.js';
import { selectRegion } from './region.js';
import { AudioEngine, clockNow } from './audio-engine.js';
import { initEditor, openSession, refreshSessions, onExportMessage } from './editor.js';
import { Compositor, ScreenRelay } from './compositor.js';

const KIND_SCREEN = 3;
const KIND_USBCAM = 5;
const KIND_LIVE = 6;
const VIRTUAL = /blackhole|loopback|vb-audio|cable|soundflower|voicemeeter|stereo mix|what u hear|virtual|ground control|existential/i;
const MODES = {
  studio: { screen: true, guide: null },
  performance: { screen: false, guide: null },
  tutorial: { screen: true, guide: null },
  social: { screen: false, guide: 9 / 16 },
};

const engine = new AudioEngine();
const S = {
  app: null, ws: null, phone: false, cam: null, pc: null, pendingIce: [],
  mode: 'studio', rotation: 0, layout: 'pip', screenCrop: null, format: '16:9', tallRatio: 2 / 3, live: null, liveCfg: {}, splitSide: 'right', splitRatio: 1 / 3,
  camSource: 'phone', usb: { stream: null, track: null, deviceId: null, height: 1080 }, usbRec: null, pipSize: 'small', pipCorner: 'br', screenSource: null, display: null, screenRec: null, calibration: null,
  rec: null, // { id, pauses, pausedAt, cameraStartMs, cameraMarkers, screenStartMs, stopping }
  waiters: new Map(), calEdges: null, savedOnce: false,
};

// ───────────────────────────────────────────── socket
function connectWs() {
  const ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  S.ws = ws;
  ws.onmessage = (e) => { if (typeof e.data === 'string') onMessage(JSON.parse(e.data)); };
  ws.onclose = () => { if (S.ws === ws) { S.ws = null; setPhone(false); setTimeout(connectWs, 800); } };
}
const send = (msg) => { if (S.ws && S.ws.readyState === 1) S.ws.send(JSON.stringify(msg)); };
const sendBin = (bytes) => { if (S.ws && S.ws.readyState === 1) S.ws.send(bytes); };

function waitFor(type, ms) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => { S.waiters.delete(type); reject(new Error(`Timed out waiting for ${type}`)); }, ms);
    S.waiters.set(type, (msg) => { clearTimeout(t); S.waiters.delete(type); resolve(msg); });
  });
}

function onMessage(msg) {
  const w = S.waiters.get(msg.type);
  if (w) w(msg);
  switch (msg.type) {
    case 'peer': setPhone(msg.connected); break;
    case 'ping': send({ type: 'pong', t0: msg.t0, td: clockNow() }); break;
    case 'rtc.offer': onOffer(msg); break;
    case 'rtc.ice':
      if (S.pc && S.pc.remoteDescription) S.pc.addIceCandidate(msg.candidate).catch(() => {});
      else S.pendingIce.push(msg.candidate);
      break;
    case 'cam.state': if (S.camSource !== 'usb') { S.cam = msg; renderCam(); } break;
    case 'cam.rec.started':
      if (S.rec && msg.sessionId === S.rec.id) { S.rec.cameraStartMs = msg.startMs; S.rec.cameraMime = msg.mime; }
      break;
    case 'cam.marker':
      if (S.rec && msg.sessionId === S.rec.id) S.rec.cameraMarkers.push([msg.mediaMs, msg.clockMs]);
      break;
    case 'cam.error': toast(`Phone: ${msg.message}`, 'error'); break;
    case 'cal.edge': if (S.calEdges) S.calEdges.push(msg.t); break;
    case 'rec.error':
      if (!S.waiters.size) toast(msg.message, 'error');
      for (const fn of [...S.waiters.values()]) fn({ error: msg.message });
      break;
    case 'export.progress': case 'export.done': case 'export.error': onExportMessage(msg); break;
    case 'live.started': onLiveStarted(); break;
    case 'live.stats': if (S.live) { S.live.seconds = msg.seconds; S.live.kbps = msg.kbps; renderLive(); } break;
    case 'live.ended': onLiveEnded(msg); break;
  }
}

// ───────────────────────────────────────────── phone / WebRTC preview
function setPhone(connected) {
  if (S.phone === connected) return;
  S.phone = connected;
  if (connected) {
    send({ type: 'hello', calibrated: !!S.calibration });
    if ($('pairDlg').open) $('pairDlg').close();
    toast('Phone connected', 'ok');
  } else {
    if (S.pc) { S.pc.close(); S.pc = null; }
    S.phoneStream = null;
    if (S.camSource !== 'usb') { $('preview').srcObject = null; S.cam = null; }
  }
  renderCam();
}

async function onOffer(msg) {
  if (S.pc) S.pc.close();
  const pc = new RTCPeerConnection({ iceServers: [] }); // LAN only, nothing leaves the network
  S.pc = pc;
  pc.ontrack = (e) => {
    try { e.receiver.jitterBufferTarget = 0; } catch { /* optional */ }
    S.phoneStream = e.streams[0] || new MediaStream([e.track]);
    if (S.camSource !== 'usb') $('preview').srcObject = S.phoneStream;
    renderCam();
  };
  pc.onicecandidate = (e) => { if (e.candidate) send({ type: 'rtc.ice', candidate: e.candidate }); };
  pc.onconnectionstatechange = () => renderCam();
  try {
    await pc.setRemoteDescription(msg.sdp);
    for (const c of S.pendingIce.splice(0)) pc.addIceCandidate(c).catch(() => {});
    await pc.setLocalDescription(await pc.createAnswer());
    send({ type: 'rtc.answer', sdp: pc.localDescription });
  } catch (e) {
    console.warn('WebRTC answer failed', e);
  }
}

const camSet = (patch) => (S.camSource === 'usb' ? usbSet(patch) : send({ type: 'cam.set', ...patch }));
const camSetSoon = debounce(camSet, 60);

// ───────────────────────────────────────────── USB camera (plugged into this computer)
function usbState() {
  const t = S.usb.track;
  if (!t || t.readyState !== 'live') return null;
  const s = t.getSettings(), c = t.getCapabilities ? t.getCapabilities() : {};
  const range = (r) => (r && typeof r.max === 'number' && r.max > r.min ? { min: r.min, max: r.max, step: r.step || 0.1 } : null);
  // Webcams usually expose brightness rather than exposure compensation.
  const expKey = range(c.exposureCompensation) ? 'exposureCompensation' : range(c.brightness) ? 'brightness' : null;
  S.usb.expKey = expKey;
  return {
    usb: true, label: t.label, width: s.width, height: s.height, fps: s.frameRate, requestedHeight: S.usb.height,
    caps: { zoom: range(c.zoom), exposure: expKey ? range(c[expKey]) : null, expRaw: expKey === 'brightness',
      focus: c.focusMode && c.focusMode.includes('manual') ? range(c.focusDistance) : null, torch: !!c.torch },
    zoom: s.zoom, exposure: expKey ? s[expKey] : null, focusMode: s.focusMode, torch: !!s.torch,
  };
}

async function listUsb() {
  const cams = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === 'videoinput');
  const prev = $('usbSel').value || S.usb.deviceId || S.app.prefs.usbDevice;
  $('usbSel').innerHTML = cams.length
    ? cams.map((d, i) => `<option value="${d.deviceId}">${(d.label || `Camera ${i + 1}`).replace(/</g, '&lt;')}</option>`).join('')
    : '<option value="">No camera found — plug one in</option>';
  if (cams.some((d) => d.deviceId === prev)) $('usbSel').value = prev;
  return cams.length;
}

function stopUsb() {
  if (S.usb.stream) S.usb.stream.getTracks().forEach((t) => t.stop());
  S.usb.stream = S.usb.track = null;
}

async function startUsb() {
  if (S.usbRec) return;
  stopUsb();
  const h = Number($('resSel').value), fps = Number($('fpsSel').value);
  const id = $('usbSel').value;
  try {
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: false,
      video: { ...(id ? { deviceId: { exact: id } } : {}), width: { ideal: Math.round((h * 16) / 9) }, height: { ideal: h }, frameRate: { ideal: fps } },
    });
    if (S.camSource === 'phone') { stream.getTracks().forEach((t) => t.stop()); return; }
    S.usb.stream = stream;
    S.usb.track = stream.getVideoTracks()[0];
    S.usb.track.contentHint = 'motion';
    S.usb.height = h;
    S.usb.deviceId = S.usb.track.getSettings().deviceId || id;
    S.usb.track.addEventListener('ended', () => {
      if (S.usb.stream !== stream) return;
      toast('The USB camera was disconnected.', 'error');
      stopUsb(); usbVideo().srcObject = null;
      if (S.camSource === 'usb') S.cam = null;
      renderCam();
    });
    usbVideo().srcObject = stream;
    if (S.camSource === 'usb') S.cam = usbState();
    await listUsb();
    $('usbSel').value = S.usb.deviceId;
    savePrefs();
  } catch (e) {
    if (S.camSource === 'usb') S.cam = null;
    toast(e.name === 'NotAllowedError' ? 'Camera access was blocked. Allow the camera for DAW LIVE CAM in Windows privacy settings.' : `USB camera: ${e.message}`, 'error');
  }
  renderCam();
}

async function usbSet(patch) {
  try {
    if (patch.height || patch.fps) { if (!S.usbRec) await startUsb(); return; }
    const t = S.usb.track;
    if (!t) return;
    const adv = {};
    if (patch.zoom != null) adv.zoom = patch.zoom;
    if (patch.exposure != null && S.usb.expKey) {
      if (S.usb.expKey === 'exposureCompensation') adv.exposureMode = 'continuous';
      adv[S.usb.expKey] = patch.exposure;
    }
    if (patch.focusMode) adv.focusMode = patch.focusMode;
    if (patch.focusDistance != null) { adv.focusMode = 'manual'; adv.focusDistance = patch.focusDistance; }
    if (patch.torch != null) adv.torch = !!patch.torch;
    await t.applyConstraints({ advanced: [adv] });
  } catch (e) {
    toast(`This camera does not support that setting (${e.message}).`, 'error');
  }
  if (S.camSource === 'usb') S.cam = usbState();
  renderCam();
}

/** The <video> showing the USB camera: the main one, or the second one next to the phone. */
const usbVideo = () => (S.camSource === 'both' ? $('preview2') : $('preview'));

async function setCamSource(src) {
  if (S.rec || S.camSource === src) return;
  S.camSource = src;
  setSeg($('camSrcSeg'), src);
  stopUsb();
  $('preview2').srcObject = null;
  S.cam = null;
  if (src === 'usb') {
    $('preview').srcObject = null;
    renderCam();
    await startUsb();
  } else {
    // Phone (and in "both" mode the USB camera next to it).
    $('preview').srcObject = S.phoneStream || null;
    if (S.phone) send({ type: 'hello', calibrated: !!S.calibration }); // ask the phone for its state again
    renderCam();
    if (src === 'both') await startUsb();
  }
  savePrefs();
}

function startUsbRecorder(slot = 'main') {
  if (S.camSource === 'phone' || !S.usb.stream) return;
  const mime = ['video/webm;codecs=h264', 'video/mp4;codecs=avc1', 'video/webm;codecs=vp9', 'video/webm;codecs=vp8', 'video/webm']
    .find((m) => MediaRecorder.isTypeSupported(m));
  const s = S.usb.track.getSettings();
  const short = Math.min(s.width || 1080, s.height || 1080);
  const bitrate = (short >= 2000 ? 40e6 : short >= 1000 ? 14e6 : 7e6) * ((s.frameRate || 30) > 40 ? 1.5 : 1);
  send({ type: 'usbcam.begin', mime, slot });
  const mr = new MediaRecorder(S.usb.stream, { mimeType: mime, videoBitsPerSecond: bitrate });
  let chain = Promise.resolve();
  mr.onstart = () => { if (S.rec) S.rec[slot === 'second' ? 'camera2StartMs' : 'cameraStartMs'] = clockNow(); };
  mr.ondataavailable = (e) => {
    if (!e.data.size) return;
    chain = chain.then(async () => {
      const out = new Uint8Array(4 + e.data.size);
      new DataView(out.buffer).setUint32(0, KIND_USBCAM, true);
      out.set(new Uint8Array(await e.data.arrayBuffer()), 4);
      sendBin(out);
    });
  };
  mr.start(1000);
  S.usbRec = { mr, flushed: () => chain };
  renderCam();
}

async function stopUsbRecorder() {
  const r = S.usbRec;
  S.usbRec = null;
  if (!r) return;
  if (r.mr.state !== 'inactive') await new Promise((resolve) => { r.mr.onstop = resolve; r.mr.stop(); });
  await r.flushed();
}

function renderCam() {
  const c = S.cam;
  const usb = S.camSource === 'usb', both = S.camSource === 'both';
  const usbLive = !!S.usb.stream;
  const connected = usb ? usbLive : both ? S.phone || usbLive : S.phone;
  const live = connected && (!!$('preview').srcObject || (both && usbLive));
  const recLost = S.rec && !usb && !S.phone;
  $('stageEmpty').hidden = connected;
  $('emptyTitle').textContent = usb ? 'Plug in a USB camera' : both ? 'Connect your phone and a USB camera' : 'Connect a camera';
  $('usbRow').hidden = S.camSource === 'phone';
  $('facingRow').hidden = usb;
  $('pairBtn2').hidden = usb;
  for (const b of $('camSrcSeg').querySelectorAll('button')) b.disabled = !!S.rec;
  $('usbSel').disabled = !!S.rec;
  $('camStats').hidden = $('camControls').hidden = !c;
  $('calBtn').disabled = !c || !!S.rec;
  const chip = $('chipCam');
  chip.className = `chip ${recLost ? 'warn' : connected ? 'on' : ''}`;
  chip.lastElementChild.textContent = recLost ? 'Reconnecting — phone keeps recording'
    : both ? `Phone ${S.phone && $('preview').srcObject ? 'live' : 'offline'} · USB ${usbLive ? 'live' : 'offline'}`
    : live ? (usb ? 'USB camera live' : 'Camera live') : connected ? 'Camera starting…' : 'Camera offline';
  $('camState').textContent = connected ? (c ? 'Connected' : 'Waiting for camera…') : 'Not connected';
  $('camState').className = `state ${connected ? 'on' : ''}`;
  if (c) {
    $('statBattery').textContent = usb ? 'Mains' : c.battery ? `${Math.round(c.battery.level * 100)}%${c.battery.charging ? ' ⚡' : ''}` : 'n/a';
    $('statLink').textContent = usb ? 'USB' : c.rtt != null ? `${Math.round(c.rtt)} ms` : '—';
    $('statFormat').textContent = c.width ? `${Math.min(c.width, c.height)}p${Math.round(c.fps || 0)}` : '—';
    setSeg($('facingSeg'), c.facing);
    const busy = !!S.rec;
    for (const el of [$('resSel'), $('fpsSel'), ...$('facingSeg').querySelectorAll('button')]) el.disabled = busy;
    const caps = c.caps || {};
    const range = (row, input, out, cap, value, fmt) => {
      row.hidden = !cap;
      if (!cap) return;
      input.min = cap.min; input.max = cap.max; input.step = cap.step || 0.1;
      if (document.activeElement !== input && value != null) input.value = value;
      out.textContent = fmt(Number(input.value));
    };
    range($('zoomRow'), $('zoom'), $('zoomOut'), caps.zoom, c.zoom, (v) => `${v.toFixed(1)}×`);
    range($('expRow'), $('exposure'), $('expOut'), caps.exposure, c.exposure, (v) => (caps.expRaw ? String(Math.round(v)) : `${v > 0 ? '+' : ''}${v.toFixed(1)} EV`));
    $('focusRow').hidden = !caps.focus;
    if (caps.focus) {
      $('focus').min = caps.focus.min; $('focus').max = caps.focus.max; $('focus').step = caps.focus.step || 0.01;
      setSeg($('focusSeg'), c.focusMode === 'manual' ? 'manual' : 'continuous');
      $('focus').disabled = c.focusMode !== 'manual';
    }
    $('torchBtn').hidden = !caps.torch;
    $('torchBtn').classList.toggle('active', !!c.torch);
    const limited = !caps.zoom && !caps.exposure && !caps.focus;
    $('camHint').textContent = [
      c.requestedHeight && c.height && Math.min(c.width, c.height) < c.requestedHeight ? `This camera delivers ${Math.min(c.width, c.height)}p at most in the browser.` : '',
      limited ? (usb ? 'This webcam does not offer zoom, focus or exposure controls to apps.' : 'This phone browser does not expose zoom, focus or exposure controls; pinch and tap on the phone instead.') : '',
      c.hidden ? 'The phone page is in the background — bring it to the front.' : '',
    ].filter(Boolean).join(' ');
  }
  layoutPreview();
  renderSteps();
  if (S.live) { const { screen, cameras } = liveSources(); compositor.setSources(screen, cameras); }
  if ($('liveBtn')) renderLive();
}

/** True when the DAW screen is live: it becomes the main picture with the camera boxed. */
const composite = () => !!$('screenMain').srcObject && !$('screenMain').hidden;

function layoutPreview() {
  const v = $('preview'), stage = $('stage'), scr = $('screenMain');
  const sw = stage.clientWidth, sh = stage.clientHeight;
  const rot = S.rotation % 180 !== 0;
  const comp = composite();
  const v2 = $('preview2');
  const two = S.camSource === 'both' && !!v2.srcObject && !!v.srcObject;
  const oneCam = !two && S.camSource === 'both' && !!v2.srcObject && !v.srcObject; // only the USB camera so far
  const multi = comp || two;
  const split = comp && S.layout === 'split';
  const tallFrame = S.format === '9:16';
  setSeg($('formatStudio'), S.format);
  $('mobileSplitBtn').hidden = !comp;
  v2.hidden = !(two || oneCam);
  stage.classList.toggle('composite', multi || oneCam);
  stage.classList.toggle('split', split || (!comp && (two || oneCam)));
  $('layoutRowStudio').hidden = !comp;
  $('igSplitBtn').hidden = !comp;
  $('regionRowStudio').hidden = !comp;
  $('regionBtn').textContent = S.screenCrop ? '▭ DAW part: chosen — change…' : '▭ Choose DAW part…';
  $('regionFull').disabled = !S.screenCrop;
  $('camSizeRowStudio').hidden = !comp;
  $('cornerRowStudio').hidden = !comp || split;
  $('divStudio').hidden = !split;
  S.frame = null;
  if (comp && document.activeElement !== $('camSizeStudio')) {
    $('camSizeStudio').value = split ? toSlider('split', tallFrame ? S.tallRatio : S.splitRatio) : toSlider('pip', pipFraction(S.pipSize));
  }
  $('camSizeOutStudio').textContent = split ? (tallFrame ? `${Math.round(S.tallRatio * 100)}% of the height` : `${Math.round(S.splitRatio * 100)}% of the width`)
    : `${Math.round(pipFraction(S.pipSize) * 100)}% of the ${tallFrame ? 'width' : 'height'}`;
  $('swapStudio').hidden = !split;
  setSeg($('layoutStudio'), S.layout);
  setSeg($('pipCornerStudio'), S.pipCorner);
  if (multi || oneCam) {
    // The frame of the final video (16:9 or 9:16), exactly as it will be exported.
    const ar = tallFrame ? 9 / 16 : 16 / 9;
    const cw = Math.min(sw, sh * ar), ch = cw / ar;
    const cx = (sw - cw) / 2, cy = (sh - ch) / 2;
    S.frame = { cx, cy, cw, ch };
    const els = two ? [v, v2] : oneCam ? [v2] : [v];
    const F = frameBoxes(cw, ch, { screen: comp, layout: S.layout, pipSize: S.pipSize, pipCorner: S.pipCorner, splitSide: S.splitSide, splitRatio: S.splitRatio, tallRatio: S.tallRatio }, els.length);
    if (F.scr) placeScreen(scr, F.scr, S.screenCrop, cx, cy);
    els.forEach((el, i) => placeVideo(el, F.cams[i], el === v ? S.rotation : 0, cx, cy));
    $('divStudio').classList.toggle('horiz', split && !F.wide);
    if (split && F.wide) Object.assign($('divStudio').style, { left: `${cx + (F.scr.x > 0 ? F.scr.x : F.scr.w)}px`, top: `${cy}px`, height: `${ch}px`, width: '' });
    else if (split) Object.assign($('divStudio').style, { left: `${cx}px`, width: `${cw}px`, top: `${cy + (F.scr.y > 0 ? F.scr.y : F.scr.h)}px`, height: '' });
    $('rotOut').textContent = `${S.rotation}°`;
    $('guide').hidden = true;
    return;
  }
  v.style.left = v.style.top = '';
  v.style.width = `${rot ? sh : sw}px`;
  v.style.height = `${rot ? sw : sh}px`;
  v.style.transform = `rotate(${S.rotation}deg)`;
  $('rotOut').textContent = `${S.rotation}°`;
  // Framing guide for vertical social formats.
  const g = $('guide'), target = MODES[S.mode].guide || (S.format === '9:16' ? 9 / 16 : null);
  let vw = v.videoWidth, vh = v.videoHeight;
  if (rot) [vw, vh] = [vh, vw];
  if (!target || !vw || vw / vh <= target + 0.01) { g.hidden = true; return; }
  const scale = Math.min(sw / vw, sh / vh);
  const dh = vh * scale;
  g.hidden = false;
  g.style.height = `${dh}px`;
  g.style.width = `${dh * target}px`;
  g.dataset.label = '9:16 SAFE AREA';
}

// ───────────────────────────────────────────── audio / screen capture
async function listDevices() {
  const devs = (await navigator.mediaDevices.enumerateDevices()).filter((d) => d.kind === 'audioinput' && d.deviceId !== 'communications');
  const systemOk = !S.app.electron || S.app.platform === 'win32';
  const prev = $('srcSel').value || S.app.prefs.source;
  const virtual = devs.filter((d) => VIRTUAL.test(d.label));
  const others = devs.filter((d) => !VIRTUAL.test(d.label));
  const opt = (v, t) => `<option value="${v}">${t.replace(/</g, '&lt;')}</option>`;
  $('srcSel').innerHTML =
    (systemOk ? opt('system', 'System audio — everything the computer plays') : '') +
    (virtual.length ? `<optgroup label="Virtual drivers / loopback">${virtual.map((d) => opt(d.deviceId, d.label)).join('')}</optgroup>` : '') +
    `<optgroup label="Audio inputs">${others.map((d, i) => opt(d.deviceId, d.label || `Input ${i + 1}`)).join('')}</optgroup>`;
  const values = [...$('srcSel').options].map((o) => o.value);
  $('srcSel').value = values.includes(prev) ? prev
    : S.app.platform === 'win32' && systemOk ? 'system' : virtual[0] ? virtual[0].deviceId : values[0] || '';
  const micPrev = $('micSel').value;
  $('micSel').innerHTML = others.map((d, i) => opt(d.deviceId, d.label || `Input ${i + 1}`)).join('');
  if ([...$('micSel').options].some((o) => o.value === micPrev)) $('micSel').value = micPrev;
  if (S.app.platform === 'darwin' && !virtual.length && !engine.running) {
    $('audioHint').textContent = 'macOS needs a loopback driver (e.g. BlackHole) to capture DAW output — see Routing help.';
  }
}

const RAW = { echoCancellation: false, noiseSuppression: false, autoGainControl: false };

function stopDisplay() {
  if (S.display) S.display.getTracks().forEach((t) => t.stop());
  S.display = null;
  if (S.relay) { S.relay.stop(); S.relay = null; }
  $('screenMain').srcObject = null;
  layoutPreview();
}

/** The steady DAW screen track (see ScreenRelay), or null. */
const screenTrack = () => (S.relay && S.relay.live ? S.relay.track : null);

function useScreenTrack(vt) {
  vt.contentHint = 'detail';
  if (!window.dlcNative) $('screenSrcName').textContent = vt.label || 'Shared screen';
  if (S.relay) S.relay.switchTo(vt);
  else {
    S.relay = new ScreenRelay(vt);
    S.relay.onSourceEnded = () => {
      // The captured window was closed: keep the last picture and ask for another.
      toast('The captured window or screen is gone — choose another with “Change”.', 'error');
      if (window.dlcNative && (S.rec || S.live)) window.dlcNative.show();
    };
    $('screenMain').srcObject = new MediaStream([S.relay.track]);
  }
  if (S.rec && !S.screenRec) startScreenRecorder(); // screen added during a recording
  if (S.live) { const { screen, cameras } = liveSources(); compositor.setSources(screen, cameras); }
  renderAudio();
}

/** Pick another screen or window — live, without stopping audio, recording or stream. */
async function switchScreen() {
  try {
    if (window.dlcNative) {
      const id = await pickScreenSource();
      if (!id) return;
      await window.dlcNative.selectSource(id);
    }
    const ds = await navigator.mediaDevices.getDisplayMedia({ video: { frameRate: 30, width: { max: 3840 }, height: { max: 2160 } }, audio: false });
    const vt = ds.getVideoTracks()[0];
    if (!vt) return;
    $('screenOn').checked = true;
    useScreenTrack(vt);
    toast('Now capturing the new screen.', 'ok');
  } catch (e) {
    if (e.name !== 'NotAllowedError') toast(e.message || String(e), 'error');
  } finally {
    // Back to the DAW: hide the dashboard again if a session is running.
    if (window.dlcNative && (S.rec || S.live) && $('autoHide').checked) window.dlcNative.hide();
  }
}

// ───────────────────────────────────────────── what to record: DAW window or screen
const DAW_WINDOW = /ableton|fl studio|fl64|logic pro|pro tools|cubase|studio one|reaper|bitwig/i;

async function sourceStillThere(id) {
  const list = await window.dlcNative.listSources().catch(() => []);
  const hit = list.find((x) => x.id === id);
  if (hit) $('screenSrcName').textContent = `${hit.kind === 'screen' ? 'Screen' : 'Window'}: ${hit.name}`;
  return !!hit;
}

/** Electron only: choose the DAW window (or a screen) with thumbnails. */
async function pickScreenSource() {
  const list = await window.dlcNative.listSources();
  if (!list.length) throw new Error('Nothing to record was found on this computer.');
  const dlg = $('srcDlg'), grid = $('srcGrid');
  // Default: the whole screen, so a full-screen DAW is captured completely.
  let chosen = (list.find((x) => x.kind === 'screen') || list[0]).id;
  grid.innerHTML = '';
  for (const src of list) {
    const b = document.createElement('button');
    const daw = src.kind === 'window' && DAW_WINDOW.test(src.name);
    b.className = (src.id === chosen ? 'active ' : '') + (daw ? 'daw' : '');
    const img = document.createElement('img');
    img.src = src.thumb;
    img.alt = '';
    const name = document.createElement('span');
    name.textContent = src.name;
    const kind = document.createElement('small');
    kind.textContent = daw ? 'DAW window' : src.kind === 'screen' ? 'Whole screen' : 'Window';
    b.append(img, name, kind);
    b.onclick = () => { chosen = src.id; for (const x of grid.children) x.classList.toggle('active', x === b); };
    b.ondblclick = () => { chosen = src.id; dlg.close('ok'); };
    grid.append(b);
  }
  $('srcOk').disabled = false;
  return new Promise((resolve) => {
    $('srcOk').onclick = () => dlg.close('ok');
    $('srcCancel').onclick = () => dlg.close('');
    dlg.addEventListener('close', () => {
      if (dlg.returnValue !== 'ok') return resolve(null);
      const src = list.find((x) => x.id === chosen);
      S.screenSource = chosen;
      $('screenSrcName').textContent = `${src.kind === 'screen' ? 'Screen' : 'Window'}: ${src.name}`;
      savePrefs();
      resolve(chosen);
    }, { once: true });
    dlg.returnValue = '';
    dlg.showModal();
  });
}

async function connectAudio() {
  const source = $('srcSel').value;
  const sampleRate = Number($('rateSel').value);
  const wantScreen = $('screenOn').checked;
  $('audioBtn').disabled = true;
  try {
    await engine.stop();
    stopDisplay();
    if (!source) throw new Error('No audio source available.');
    let dawStream;
    if (source === 'system' || wantScreen) {
      if (wantScreen && window.dlcNative) {
        const id = S.screenSource && (await sourceStillThere(S.screenSource)) ? S.screenSource : await pickScreenSource();
        if (!id) return; // cancelled
        await window.dlcNative.selectSource(id);
      }
      const display = await navigator.mediaDevices.getDisplayMedia({
        video: { frameRate: 30, width: { max: 3840 }, height: { max: 2160 } },
        audio: source === 'system' ? RAW : false, systemAudio: 'include',
      });
      S.display = display;
      if (source === 'system') {
        if (!display.getAudioTracks().length) {
          throw new Error('No audio was shared. Pick the screen again and enable “Share system audio”.');
        }
        dawStream = new MediaStream(display.getAudioTracks());
      }
      const vt = display.getVideoTracks()[0];
      if (vt) display.removeTrack(vt); // the video lives in the screen relay from here on
      if (wantScreen && vt) useScreenTrack(vt);
      else if (vt) vt.stop();
    }
    if (!dawStream) {
      dawStream = await navigator.mediaDevices.getUserMedia({
        audio: { deviceId: { exact: source }, ...RAW, channelCount: { ideal: 2 }, sampleRate: { ideal: sampleRate } },
      });
    }
    let micStream = null;
    if ($('micOn').checked && $('micSel').value) {
      micStream = await navigator.mediaDevices.getUserMedia({ audio: { deviceId: { exact: $('micSel').value }, ...RAW } });
    }
    await engine.start({ dawStream, micStream, sampleRate });
    savePrefs();
    await listDevices(); // labels become available once permission is granted
  } catch (e) {
    await engine.stop();
    stopDisplay();
    if (e.name !== 'NotAllowedError' || !/dismiss|denied/i.test(e.message)) toast(e.message || String(e), 'error');
  } finally {
    $('audioBtn').disabled = false;
    renderAudio();
  }
}

function renderAudio() {
  if (S.live) { const { screen, cameras } = liveSources(); compositor.setSources(screen, cameras); }
  const on = engine.running;
  const screen = !!screenTrack();
  $('chipAudio').className = `chip ${on ? 'on' : ''}`;
  $('chipAudio').lastElementChild.textContent = on ? `DAW audio · ${engine.sampleRate / 1000} kHz` : 'Audio offline';
  $('chipScreen').hidden = !screen;
  $('screenMain').hidden = !screen;
  $('screenSrcRow').hidden = !((window.dlcNative && $('screenOn').checked) || screenTrack());
  $('liveScreenBtn').disabled = !engine.running;
  layoutPreview();
  $('audioState').textContent = on ? 'Connected' : 'Not connected';
  $('audioState').className = `state ${on ? 'on' : ''}`;
  $('audioBtn').textContent = on ? 'Disconnect' : 'Connect DAW audio';
  $('audioBtn').classList.toggle('primary', !on);
  const lock = on || !!S.rec;
  for (const id of ['srcSel', 'rateSel', 'bitSel', 'micOn', 'micSel', 'screenOn']) $(id).disabled = lock;
  $('audioBtn').disabled = !!S.rec;
  $('micRow').hidden = !$('micOn').checked;
  if (on) $('audioHint').textContent = 'Play your DAW — the meter should move. Disconnect to change settings.';
  $('recBtn').disabled = !on && !S.rec;
  renderSteps();
}

engine.addEventListener('lost', () => {
  if (S.rec) { toast('The audio source stopped. Recording was ended.', 'error'); stopRecording(); }
  else { engine.stop().then(() => { stopDisplay(); renderAudio(); toast('The audio source was disconnected.', 'error'); }); }
});

// ───────────────────────────────────────────── meters & waveform
const M = { peak: [0, 0], rms: [0, 0], hold: [0, 0], holdAt: [0, 0], clipAt: 0, wave: new Float32Array(2048), wavePos: 0, waveRec: new Uint8Array(2048) };
engine.addEventListener('levels', (e) => {
  const { peak, rms } = e.detail;
  const now = performance.now();
  for (let i = 0; i < 2; i++) {
    M.peak[i] = Math.max(peak[i], M.peak[i]);
    M.rms[i] = Math.max(rms[i], M.rms[i]);
    if (peak[i] >= M.hold[i] || now - M.holdAt[i] > 1500) { M.hold[i] = peak[i]; M.holdAt[i] = now; }
    if (peak[i] >= 0.999) M.clipAt = now;
  }
  M.wave[M.wavePos] = Math.max(peak[0], peak[1]);
  M.waveRec[M.wavePos] = S.rec ? (S.rec.pausedAt != null ? 2 : 1) : 0;
  M.wavePos = (M.wavePos + 1) % M.wave.length;
});

function drawMeters() {
  const { ctx, w, h } = fitCanvas($('meter'));
  ctx.clearRect(0, 0, w, h);
  const y = (v) => { const d = Math.max(-60, dB(v)); return h - ((d + 60) / 60) * h; };
  const barW = 14, x0 = 0;
  const grad = ctx.createLinearGradient(0, h, 0, 0);
  grad.addColorStop(0, '#1fd6a0'); grad.addColorStop(0.7, '#3dffa0'); grad.addColorStop(0.85, '#ffb648'); grad.addColorStop(1, '#ff3b5c');
  for (let i = 0; i < 2; i++) {
    const x = x0 + i * (barW + 3);
    ctx.fillStyle = '#1a1e2b'; ctx.fillRect(x, 0, barW, h);
    ctx.fillStyle = grad; ctx.fillRect(x, y(M.peak[i]), barW, h - y(M.peak[i]));
    ctx.fillStyle = '#ffffff55'; ctx.fillRect(x, y(M.rms[i]), barW, 1.5);
    if (M.hold[i] > 0.001) { ctx.fillStyle = M.hold[i] >= 0.999 ? '#ff3b5c' : '#e9ebf3'; ctx.fillRect(x, Math.max(0, y(M.hold[i]) - 1), barW, 2); }
    M.peak[i] *= 0.86; M.rms[i] *= 0.9; // release ballistics
  }
  ctx.fillStyle = '#5b6178'; ctx.font = '8px ui-monospace, monospace'; ctx.textBaseline = 'middle';
  for (const d of [0, -6, -12, -24, -48]) {
    const yy = Math.min(h - 5, Math.max(5, h - ((d + 60) / 60) * h));
    ctx.fillRect(33, Math.round(h - ((d + 60) / 60) * h), 3, 1);
    ctx.fillText(String(d), 38, yy);
  }
  const hold = Math.max(M.hold[0], M.hold[1]);
  $('peakRead').textContent = hold > 0.001 ? dB(hold).toFixed(1) : '−∞';
  $('peakRead').parentElement.classList.toggle('clip', performance.now() - M.clipAt < 2000);
}

function drawWave() {
  const { ctx, w, h } = fitCanvas($('wave'));
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = '#232736'; ctx.fillRect(0, h / 2, w, 1);
  const step = 3, n = Math.min(M.wave.length, Math.floor(w / step));
  for (let i = 0; i < n; i++) {
    const idx = (M.wavePos - 1 - i + M.wave.length * 2) % M.wave.length;
    const a = Math.max(1, Math.pow(M.wave[idx], 0.6) * (h / 2 - 6));
    const state = M.waveRec[idx];
    ctx.fillStyle = state === 1 ? '#ff3b5c' : state === 2 ? '#ffb648' : '#2ff3e0';
    ctx.globalAlpha = 0.35 + 0.65 * (1 - i / n);
    ctx.fillRect(w - (i + 1) * step, h / 2 - a, step - 1, a * 2);
  }
  ctx.globalAlpha = 1;
}

// ───────────────────────────────────────────── recording
function startScreenRecorder() {
  if (S.screenRec) return; // one screen recorder per session: two would interleave into one file
  const tracks = screenTrack() ? [screenTrack()] : [];
  if (!tracks.length) return;
  const mime = ['video/webm;codecs=vp9', 'video/webm;codecs=vp8', 'video/webm'].find((m) => MediaRecorder.isTypeSupported(m));
  const mr = new MediaRecorder(new MediaStream(tracks), { mimeType: mime, videoBitsPerSecond: 10_000_000 });
  let chain = Promise.resolve();
  mr.onstart = () => { if (S.rec) S.rec.screenStartMs = clockNow(); };
  mr.ondataavailable = (e) => {
    if (!e.data.size) return;
    chain = chain.then(async () => {
      const out = new Uint8Array(4 + e.data.size);
      new DataView(out.buffer).setUint32(0, KIND_SCREEN, true);
      out.set(new Uint8Array(await e.data.arrayBuffer()), 4);
      sendBin(out);
    });
  };
  mr.start(1000);
  S.screenRec = { mr, flushed: () => chain };
}

async function stopScreenRecorder() {
  const sr = S.screenRec;
  S.screenRec = null;
  if (!sr) return;
  if (sr.mr.state !== 'inactive') {
    await new Promise((resolve) => { sr.mr.onstop = resolve; sr.mr.stop(); });
  }
  await sr.flushed();
}

async function startRecording() {
  if (S.rec || S.preRoll || !engine.running) return;
  $('recBtn').disabled = true;
  try {
    if (window.dlcNative && $('autoHide').checked) {
      S.preRoll = true;
      syncBusy(); // minimise first…
      await sleep(700); // …and let it get out of the picture before the first frame
    }
    const started = waitFor('rec.started', 5000);
    send({
      type: 'rec.start', title: $('titleInput').value.trim() || defaultTitle(), mode: S.mode,
      audio: { sampleRate: engine.sampleRate, bitDepth: Number($('bitSel').value) }, mic: engine.hasMic,
    });
    const res = await started;
    if (res.error) throw new Error(res.error);
    S.rec = { id: res.id, pauses: [], pausedAt: null, cameraStartMs: null, cameraMarkers: [], screenStartMs: null };
    engine.startRecording(sendBin);
    startScreenRecorder();
    syncBusy();
    let cams = 0;
    if (S.camSource === 'usb' && S.usb.stream) { startUsbRecorder('main'); cams++; }
    if (S.camSource !== 'usb' && S.phone) { send({ type: 'cam.rec.start', sessionId: res.id }); cams++; }
    if (S.camSource === 'both' && S.usb.stream) { startUsbRecorder('second'); cams++; }
    if (!cams) toast('No camera connected — recording audio' + (S.screenRec ? ' and screen' : '') + ' only.');
  } catch (e) {
    toast(e.message, 'error');
  } finally {
    S.preRoll = false;
    syncBusy();
  }
  renderRec();
}

async function stopRecording() {
  const r = S.rec;
  if (!r || r.stopping) return;
  r.stopping = true;
  renderRec();
  if (r.pausedAt != null) { r.pauses.push([r.pausedAt, engine.recordedSeconds()]); r.pausedAt = null; }
  if (S.camSource !== 'usb') send({ type: 'cam.rec.stop', sessionId: r.id });
  const audio = await engine.stopRecording();
  await Promise.all([stopScreenRecorder(), stopUsbRecorder()]);
  const saved = waitFor('rec.saved', 90000);
  const c = S.cam || {};
  send({
    type: 'rec.stop',
    timing: {
      audioStartMs: audio.startMs, audioMarkers: audio.markers, cameraStartMs: r.cameraStartMs,
      cameraMarkers: r.cameraMarkers, screenStartMs: r.screenStartMs, camera2StartMs: r.camera2StartMs, pauses: r.pauses, calibration: calibrationFor(),
    },
    camera: { width: c.width, height: c.height, fps: Math.round(c.fps || 30), rotation: S.rotation },
    camera2: S.camSource === 'both' && S.usb.track ? (({ width, height }) => ({ width, height }))(S.usb.track.getSettings()) : null,
    preset: { aspect: S.format, tallRatio: S.format === '9:16' ? S.tallRatio : null, pipSize: S.pipSize, pipCorner: S.pipCorner, splitSide: S.splitSide, splitRatio: S.splitRatio, screenCrop: S.screenCrop, ...(r.screenStartMs != null ? { layout: S.layout } : {}) },
  });
  toast('Saving session…');
  try {
    const res = await saved;
    if (res.error) throw new Error(res.error);
    S.savedOnce = true;
    toast('Session saved', 'ok');
    S.rec = null;
    renderRec();
    syncBusy();
    showView('library');
    await refreshSessions();
    openSession(res.id);
  } catch (e) {
    toast(`Could not save the session: ${e.message}`, 'error');
    S.rec = null;
    renderRec();
    syncBusy();
  }
  refreshState();
}

function togglePause() {
  const r = S.rec;
  if (!r || r.stopping) return;
  if (r.pausedAt == null) r.pausedAt = engine.recordedSeconds();
  else { r.pauses.push([r.pausedAt, engine.recordedSeconds()]); r.pausedAt = null; }
  if (S.camSource !== 'usb') send({ type: 'cam.rec.pause', paused: r.pausedAt != null });
  renderRec();
}

function elapsed() {
  const r = S.rec;
  if (!r) return 0;
  const now = r.pausedAt != null ? r.pausedAt : engine.recordedSeconds();
  return now - r.pauses.reduce((s, [a, b]) => s + (b - a), 0);
}

function defaultTitle() {
  const d = new Date();
  return `Session ${d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' })} ${d.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' })}`;
}

function renderRec() {
  const r = S.rec;
  const paused = !!r && r.pausedAt != null;
  $('recBtn').classList.toggle('recording', !!r);
  $('recBtn').disabled = r ? !!r.stopping : !engine.running;
  $('recBtn').setAttribute('aria-label', r ? 'Stop recording' : 'Record');
  $('pauseBtn').disabled = !r || !!r.stopping;
  $('pauseBtn').classList.toggle('paused', paused);
  $('recBadge').hidden = !r;
  $('recBadge').classList.toggle('paused', paused);
  $('recBadge').lastElementChild.textContent = paused ? 'PAUSED' : 'REC';
  $('timer').classList.toggle('live', !!r && !paused);
  for (const b of $('modes').querySelectorAll('button')) b.disabled = !!r;
  $('titleInput').disabled = !!r;
  renderAudio();
  renderCam();
}

// ───────────────────────────────────────────── live streaming (Instagram / TikTok / RTMP)
const PLATFORMS = {
  instagram: {
    placeholder: 'rtmps://edgetee-upload-….fbcdn.net:443/rtmp/',
    help: 'On instagram.com (computer): Create → Live video → choose title and audience → copy the <b>Stream URL</b> and <b>Stream key</b> here. Press Go live here first, then Go live on Instagram once its preview shows you. The key is new for every broadcast.',
    format: '9:16',
  },
  tiktok: {
    placeholder: 'rtmp://push-rtmp-….tiktokcdn.com/game/',
    help: 'Needs a TikTok account with LIVE access and a stream key (TikTok LIVE Center → LIVE → Stream key, or LIVE Studio). Copy the <b>Server URL</b> and <b>Stream key</b> here, press Go live here, then Go live in TikTok. Not every account is offered a stream key.',
    format: '9:16',
  },
  custom: {
    placeholder: 'rtmp://a.rtmp.youtube.com/live2',
    help: 'Any RTMP or RTMPS server (YouTube, Twitch, Facebook, Restream…): paste the server URL and stream key from the platform.',
    format: '16:9',
  },
};
const compositor = new Compositor($('livePreview'));

function liveLayout() {
  // On a vertical stream the DAW goes across the top and you fill the rest,
  // unless the studio picture was chosen.
  if (S.live && S.live.height > S.live.width && S.live.cfg.vert !== 'studio') {
    return { layout: 'split', splitSide: 'right', splitRatio: S.splitRatio, pipSize: S.pipSize, pipCorner: S.pipCorner, rotation: S.rotation, screenCrop: S.screenCrop };
  }
  return { layout: S.layout, pipSize: S.pipSize, pipCorner: S.pipCorner, splitSide: S.splitSide, splitRatio: S.splitRatio, tallRatio: S.format === '9:16' ? S.tallRatio : undefined, rotation: S.rotation, screenCrop: S.screenCrop };
}
function liveSources() {
  const screen = screenTrack() ? S.relay : null;
  const live = (st) => (st ? st.getVideoTracks().find((t) => t.readyState === 'live') : null);
  const cameras = S.camSource === 'usb' ? [live(S.usb.stream)]
    : S.camSource === 'both' ? [live(S.phoneStream), live(S.usb.stream)] : [live(S.phoneStream)];
  return { screen, cameras: cameras.filter(Boolean), camera: cameras.find(Boolean) || null };
}

function readLiveForm() {
  const plat = S.liveCfg.platform || 'instagram';
  return { platform: plat, server: $('liveServer').value.trim(), key: $('liveKey').value.trim(), format: $('liveFormat').value, quality: Number($('liveQual').value), vert: $('liveVert').value };
}

function renderLive() {
  const L = S.live;
  const plat = S.liveCfg.platform || 'instagram';
  setSeg($('livePlat'), plat);
  $('liveServer').placeholder = PLATFORMS[plat].placeholder;
  $('liveHelp').innerHTML = PLATFORMS[plat].help;
  const { screen, camera } = liveSources();
  const ready = engine.running && (screen || camera);
  $('liveBtn').disabled = !L && (!ready || !$('liveServer').value.trim());
  $('liveBtn').classList.toggle('on', !!L);
  $('liveBtn').textContent = L ? '■ End live' : '● Go live';
  for (const id of ['liveServer', 'liveKey', 'liveFormat', 'liveQual']) $(id).disabled = !!L;
  $('liveVertRow').hidden = $('liveFormat').value !== '9:16';
  for (const b of $('livePlat').querySelectorAll('button')) b.disabled = !!L;
  $('livePreview').hidden = !L;
  $('liveState').className = `state ${L && L.on ? 'live' : ''}`;
  $('liveState').textContent = !L ? 'Off' : L.on ? `LIVE ${fmtClock(L.seconds || 0)}` : L.retry ? 'Reconnecting…' : 'Connecting…';
  $('liveInfo').textContent = !L ? (ready ? '' : 'Connect DAW audio and a camera or the DAW screen first.')
    : L.on ? `Sending ${L.kbps || 0} kbps · ${L.width}×${L.height}` : L.retry ? `Connection lost — retry ${L.retry} of 5…` : 'Connecting to the platform…';
  syncBusy();
}

function startLiveEncoder() {
  const L = S.live;
  const { screen, cameras } = liveSources();
  compositor.setSources(screen, cameras);
  const video = compositor.start({ width: L.width, height: L.height, fps: 30, getLayout: liveLayout });
  const audio = engine.liveAudio();
  const tracks = [video, ...(audio ? audio.getAudioTracks() : [])];
  const mime = ['video/webm;codecs=h264,opus', 'video/webm;codecs=vp8,opus', 'video/webm'].find((m) => MediaRecorder.isTypeSupported(m));
  const mr = new MediaRecorder(new MediaStream(tracks), { mimeType: mime, videoBitsPerSecond: 12_000_000, audioBitsPerSecond: 192_000 });
  let chain = Promise.resolve();
  mr.ondataavailable = (e) => {
    if (!e.data.size) return;
    chain = chain.then(async () => {
      const out = new Uint8Array(4 + e.data.size);
      new DataView(out.buffer).setUint32(0, KIND_LIVE, true);
      out.set(new Uint8Array(await e.data.arrayBuffer()), 4);
      sendBin(out);
    });
  };
  L.mr = mr;
  send({ type: 'live.start', server: L.cfg.server, key: L.cfg.key, width: L.width, height: L.height, fps: 30, bitrateKbps: L.kbpsTarget });
}

function onLiveStarted() {
  const L = S.live;
  if (!L || !L.mr || L.mr.state !== 'inactive') return;
  L.mr.start(250);
  L.on = true;
  renderLive();
}

function stopLiveEncoder() {
  const L = S.live;
  if (L && L.mr && L.mr.state !== 'inactive') L.mr.stop();
  compositor.stop();
}

async function goLive() {
  if (S.live) return;
  const cfg = readLiveForm();
  if (!/^rtmps?:\/\//i.test(cfg.server)) { toast('The server URL must start with rtmp:// or rtmps://', 'error'); return; }
  if (!cfg.key && !/\/[^/]+\/[^/]+$/.test(new URL(cfg.server).pathname)) toast('No stream key entered — using the server URL as it is.');
  saveLiveCfg();
  const q = cfg.quality === 1080 ? { s: 1080, kbps: 6000 } : { s: 720, kbps: 4000 };
  const vertical = cfg.format === '9:16';
  if (window.dlcNative && $('autoHide').checked && !S.rec) { S.preRoll = true; syncBusy(); await sleep(700); S.preRoll = false; }
  S.live = { cfg, width: vertical ? q.s : Math.round((q.s * 16) / 9 / 2) * 2, height: vertical ? Math.round((q.s * 16) / 9 / 2) * 2 : q.s, kbpsTarget: q.kbps, on: false, retry: 0, wanted: true };
  startLiveEncoder();
  renderLive();
}

function stopLive() {
  const L = S.live;
  if (!L) return;
  L.wanted = false;
  send({ type: 'live.stop' });
  stopLiveEncoder();
  S.live = null;
  renderLive();
}

function onLiveEnded(msg) {
  const L = S.live;
  if (!L) return;
  stopLiveEncoder();
  if (L.wanted && !msg.ok && L.retry < 5) {
    // Automatic reconnect: the platform or Wi-Fi dropped us.
    L.retry += 1;
    L.on = false;
    toast(`Live: ${msg.message} Reconnecting…`, 'error');
    renderLive();
    setTimeout(() => { if (S.live === L && L.wanted) startLiveEncoder(); }, 2000 * L.retry);
    return;
  }
  S.live = null;
  toast(msg.ok ? 'Live stream ended.' : `Live stream stopped: ${msg.message}`, msg.ok ? 'ok' : 'error');
  renderLive();
}

function saveLiveCfg() {
  const plat = S.liveCfg.platform || 'instagram';
  S.liveCfg[plat] = { server: $('liveServer').value.trim(), key: $('liveKey').value.trim(), format: $('liveFormat').value, quality: $('liveQual').value, vert: $('liveVert').value };
  api('/api/settings', 'PUT', { live: S.liveCfg }).catch(() => {});
}
function loadLivePlatform(plat) {
  S.liveCfg.platform = plat;
  const c = S.liveCfg[plat] || {};
  $('liveServer').value = c.server || '';
  $('liveKey').value = c.key || '';
  $('liveFormat').value = c.format || PLATFORMS[plat].format;
  $('liveQual').value = c.quality || '720';
  $('liveVert').value = c.vert || 'stack';
  renderLive();
}

// ───────────────────────────────────────────── DAW region & Instagram split preset
/** Shape (width/height) of the panel the DAW fills in the current 16:9 picture. */
function dawPanelAspect() {
  const n = S.camSource === 'both' ? 2 : 1;
  const [W, H] = S.format === '9:16' ? [1080, 1920] : [1920, 1080];
  const F = frameBoxes(W, H, { screen: true, layout: S.layout, pipSize: S.pipSize, pipCorner: S.pipCorner, splitSide: S.splitSide, splitRatio: S.splitRatio, tallRatio: S.format === '9:16' ? S.tallRatio : undefined }, n);
  return F.scr.w / F.scr.h;
}

async function chooseRegion() {
  if (!S.relay || !S.relay.live) { toast('Connect DAW audio with “Capture the DAW screen” ticked first.', 'error'); return; }
  const relay = S.relay;
  const result = await selectRegion({
    source: () => (relay.live ? { src: relay.canvas, w: relay.width, h: relay.height } : null),
    aspect: dawPanelAspect(), initial: S.screenCrop,
  });
  if (result === undefined) return;
  S.screenCrop = validCrop(result);
  layoutPreview();
  savePrefs();
}

/** Instagram 16:9: phone camera in the left 2/3, the DAW timeline in the right 1/3. */
async function instagramSplit() {
  S.format = '16:9';
  S.layout = 'split';
  S.splitSide = 'left';
  S.splitRatio = 2 / 3;
  if (S.camSource === 'both') await setCamSource('phone');
  if (!S.live) {
    saveLiveCfg();
    S.liveCfg.platform = 'instagram';
    S.liveCfg.instagram = { ...(S.liveCfg.instagram || {}), format: '16:9' };
    loadLivePlatform('instagram');
    saveLiveCfg();
  }
  layoutPreview();
  savePrefs();
  toast('Instagram 16:9: phone 2/3 + DAW 1/3. Now choose the timeline part.', 'ok');
  if (S.relay && S.relay.live) await chooseRegion();
}

/** Mobile 9:16 recording: phone camera on top, the DAW (or a part of it) below. */
async function mobileSplit() {
  S.format = '9:16';
  S.layout = 'split';
  S.splitSide = 'left'; // camera first = on top
  S.tallRatio = 2 / 3;
  if (S.camSource === 'both') await setCamSource('phone');
  if (!S.live) {
    // A live stream from here uses the same vertical picture.
    saveLiveCfg();
    const plat = S.liveCfg.platform || 'instagram';
    S.liveCfg[plat] = { ...(S.liveCfg[plat] || {}), format: '9:16', vert: 'studio' };
    loadLivePlatform(plat);
    saveLiveCfg();
  }
  layoutPreview();
  savePrefs();
  toast('Mobile 9:16: phone on top, DAW below. Drag the line to resize; choose the DAW part if you like.', 'ok');
  if (S.relay && S.relay.live) await chooseRegion();
}

// ───────────────────────────────────────────── dashboard hiding & floating controls (desktop app)
function syncBusy() {
  if (!window.dlcNative) return;
  window.dlcNative.setBusy({ recording: !!S.rec || !!S.preRoll, live: !!S.live, autoHide: $('autoHide').checked });
}
function onShellCommand(cmd) {
  if (cmd === 'toggle') { if (S.rec) stopRecording(); else startRecording(); }
  else if (cmd === 'stop') stopRecording();
  else if (cmd === 'pause') togglePause();
  else if (cmd === 'live-stop') stopLive();
  else if (cmd === 'screen') switchScreen();
}

// ───────────────────────────────────────────── workflow steps & storage
function renderSteps() {
  const done = {
    phone: S.camSource === 'usb' ? !!S.usb.stream : S.phone, daw: engine.running,
    preview: (S.camSource === 'usb' ? !!S.usb.stream : S.phone) && engine.running && !!$('preview').srcObject,
    record: S.savedOnce, export: false,
  };
  let seenNow = false;
  for (const li of $('steps').children) {
    const k = li.dataset.step;
    const isNow = !seenNow && !done[k];
    li.className = done[k] ? 'done' : isNow ? 'now' : '';
    if (isNow) seenNow = true;
  }
}

async function refreshState() {
  S.app = await api('/api/state');
  const { free, total } = S.app.storage;
  if (free != null) {
    const bytesPerSec = (engine.sampleRate || 48000) * 2 * (Number($('bitSel').value) / 8) + 2_500_000; // audio + typical video
    const mins = free / bytesPerSec / 60;
    $('storageText').textContent = `${fmtBytes(free)} free · ~${mins > 120 ? `${Math.round(mins / 60)} h` : `${Math.round(mins)} min`}`;
    $('storageFill').style.width = `${Math.round((1 - free / total) * 100)}%`;
    $('storage').classList.toggle('low', free < 5 * 2 ** 30);
  }
}

function savePrefs() {
  api('/api/settings', 'PUT', { prefs: {
    source: $('srcSel').value, sampleRate: $('rateSel').value, bitDepth: $('bitSel').value,
    mode: S.mode, rotation: S.rotation, mic: $('micOn').checked,
    pipSize: S.pipSize, pipCorner: S.pipCorner, screenSource: S.screenSource, picture: S.layout, screenCrop: S.screenCrop, format: S.format, tallRatio: S.tallRatio, autoHide: $('autoHide').checked, splitSide: S.splitSide, splitRatio: S.splitRatio, camSource: S.camSource, usbDevice: S.usb.deviceId,
  } }).catch(() => {});
}

function setMode(mode, fromUser) {
  S.mode = MODES[mode] ? mode : 'studio';
  for (const b of $('modes').querySelectorAll('button')) b.classList.toggle('active', b.dataset.mode === S.mode);
  if (!engine.running) $('screenOn').checked = MODES[S.mode].screen;
  else if (fromUser && MODES[S.mode].screen !== $('screenOn').checked) {
    toast(MODES[S.mode].screen ? 'Reconnect DAW audio with “Capture the DAW screen” ticked to include the screen.' : 'Screen capture stays on; untick it and reconnect to turn it off.');
  }
  layoutPreview();
  if (fromUser) savePrefs();
}

function showView(name) {
  for (const t of document.querySelectorAll('.tab')) t.classList.toggle('active', t.dataset.view === name);
  $('view-studio').hidden = name !== 'studio';
  $('view-library').hidden = name !== 'library';
  if (name === 'library') refreshSessions();
  else requestAnimationFrame(layoutPreview);
}

// ───────────────────────────────────────────── pairing dialog
async function openPair() {
  await refreshState();
  const urls = S.app.pairUrls;
  const sel = $('pairNet');
  sel.innerHTML = urls.map((u, i) => `<option value="${i}">${u.address} (${u.name})</option>`).join('');
  $('pairNetRow').hidden = urls.length < 2;
  const show = async () => {
    const u = urls[Number(sel.value) || 0];
    if (!u) {
      $('qr').textContent = '';
      $('pairUrl').textContent = 'No network connection found. Connect this computer to Wi-Fi or Ethernet.';
      return;
    }
    $('pairUrl').textContent = u.url;
    $('qr').innerHTML = await (await fetch(`/api/qr.svg?u=${encodeURIComponent(u.url)}`)).text();
  };
  sel.onchange = show;
  await show();
  if (!$('pairDlg').open) $('pairDlg').showModal();
}

// ───────────────────────────────────────────── sync calibration
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const median = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.floor(s.length / 2)] : null; };

function showCalibration() {
  const c = S.calibration;
  $('calResult').hidden = !c;
  if (c) {
    $('calVideo').textContent = [c.videoLatencyMs ? `phone ${Math.round(c.videoLatencyMs)} ms` : '', c.usbLatencyMs ? `USB ${Math.round(c.usbLatencyMs)} ms` : ''].filter(Boolean).join(' · ') || '—';
    $('calAudio').textContent = c.audioLatencyMs ? `${Math.round(c.audioLatencyMs)} ms` : 'not measured';
  }
}

/** Calibration values for the camera(s) in use: phone in videoLatencyMs, USB in usbLatencyMs. */
function calibrationFor() {
  const c = S.calibration;
  if (!c) return null;
  const phone = c.videoLatencyMs || 0, usb = c.usbLatencyMs || 0;
  return { audioLatencyMs: c.audioLatencyMs || 0, videoLatencyMs: S.camSource === 'usb' ? usb : phone, camera2LatencyMs: usb };
}

let flashDetect = null;
function startLocalFlashDetect(v, edges) {
  const cv = document.createElement('canvas');
  cv.width = cv.height = 24;
  const ctx = cv.getContext('2d', { willReadFrequently: true });
  const run = { base: null, last: 0, on: true };
  flashDetect = run;
  const step = (now, meta) => {
    if (!run.on) return;
    ctx.drawImage(v, 0, 0, 24, 24);
    const d = ctx.getImageData(0, 0, 24, 24).data;
    let sum = 0;
    for (let i = 0; i < d.length; i += 4) sum += d[i] * 0.299 + d[i + 1] * 0.587 + d[i + 2] * 0.114;
    const luma = sum / 576;
    // Prefer the camera's own capture time when the browser reports it.
    const t = meta && meta.captureTime ? performance.timeOrigin + meta.captureTime : clockNow();
    if (run.base == null) run.base = luma;
    if (luma - run.base > 22 && t - run.last > 400) { run.last = t; edges.push(t); }
    run.base += (luma - run.base) * (luma > run.base ? 0.02 : 0.2);
    v.requestVideoFrameCallback(step);
  };
  v.requestVideoFrameCallback(step);
}
function stopLocalFlashDetect() { if (flashDetect) flashDetect.on = false; flashDetect = null; }

async function runCalibration() {
  $('calStart').disabled = true;
  const usePhone = S.camSource !== 'usb' && S.phone;
  const useUsb = S.camSource !== 'phone' && !!S.usb.stream;
  const usbEdges = [];
  try {
    S.calEdges = [];
    if (useUsb) startLocalFlashDetect(usbVideo(), usbEdges);
    if (usePhone) send({ type: 'cal.start' });
    await sleep(900); // let the cameras settle their brightness baseline
    const flashes = [];
    const audioLat = [];
    for (let i = 0; i < 8; i++) {
      await sleep(650 + Math.random() * 350);
      const click = engine.running ? engine.measureLoopbackLatency() : null;
      $('flash').classList.add('on');
      await new Promise((r) => requestAnimationFrame(() => { flashes.push(clockNow()); r(); }));
      await sleep(140);
      $('flash').classList.remove('on');
      if (click) { const l = await click; if (l != null) audioLat.push(l); }
    }
    await sleep(700);
    stopLocalFlashDetect(); send({ type: 'cal.stop' });
    const latencies = (edges) => {
      const out = [];
      for (const f of flashes) { const e = edges.find((t) => t > f + 5 && t < f + 700); if (e != null) out.push(e - f); }
      return out;
    };
    const phoneLat = usePhone ? latencies(S.calEdges) : [];
    const usbLat = useUsb ? latencies(usbEdges) : [];
    S.calEdges = null;
    const missing = [usePhone && phoneLat.length < 4 && 'phone', useUsb && usbLat.length < 4 && 'USB camera'].filter(Boolean);
    if (missing.length) throw new Error(`The ${missing.join(' and the ')} did not see the flashes. Aim it at this screen, move closer and try again.`);
    const base = engine.running ? (engine.ctx.baseLatency || 0) * 1000 : 0;
    const prev = S.calibration || {};
    S.calibration = {
      videoLatencyMs: usePhone ? Math.round(median(phoneLat)) : prev.videoLatencyMs || 0,
      usbLatencyMs: useUsb ? Math.round(median(usbLat)) : prev.usbLatencyMs || 0,
      audioLatencyMs: audioLat.length >= 4 ? Math.max(0, Math.round(median(audioLat) - base)) : prev.audioLatencyMs || 0,
      at: new Date().toISOString(),
    };
    await api('/api/settings', 'PUT', { calibration: S.calibration });
    $('calText').textContent = 'Calibrated. New recordings are aligned with these delays automatically.';
    toast('Sync calibrated', 'ok');
  } catch (e) {
    S.calEdges = null;
    stopLocalFlashDetect(); send({ type: 'cal.stop' });
    $('calText').textContent = e.message;
  }
  $('flash').classList.remove('on');
  showCalibration();
  $('calStart').disabled = false;
}

// ───────────────────────────────────────────── DAW routing help
async function openDawGuide() {
  const daws = await api('/api/daws').catch(() => []);
  const running = daws.filter((d) => d.running);
  const where = (running.length ? running : daws).map((d) => `<li><b>${d.name}</b>: ${d.output}</li>`).join('');
  const win = S.app.platform === 'win32', mac = S.app.platform === 'darwin';
  $('dawGuide').innerHTML = `
    <p>${running.length ? `Detected: <b>${running.map((d) => d.name).join(', ')}</b>.` : 'Works with every DAW that plays through an audio device:'}</p>
    <div class="daw-grid">${daws.map((d) => `<span class="${d.running ? 'run' : ''}"><i class="dot ${d.running ? 'on' : ''}"></i>${d.name}</span>`).join('')}</div>
    ${win || !mac ? `<h4>${win ? 'WINDOWS' : 'THIS COMPUTER'} — SIMPLEST</h4>
    <ol><li>Choose <b>System audio</b> as the source and connect.</li>
    <li>Let the DAW play through the device you normally listen on (WASAPI / default output). The master output is captured digitally, exactly as you hear it.</li>
    <li>If your DAW uses an <b>exclusive ASIO</b> driver, system capture hears nothing: switch the DAW to WASAPI / FL Studio ASIO / ASIO4ALL, or use a virtual cable as below.</li></ol>` : ''}
    <h4>${mac ? 'MACOS' : 'ANY SYSTEM'} — VIRTUAL DRIVER OR LOOPBACK INPUT</h4>
    <ol><li>Install a loopback driver: ${mac ? '<b>BlackHole 2ch</b> (free) or <b>Loopback</b>' : '<b>VB-CABLE</b> or <b>VoiceMeeter</b>'}. Many audio interfaces also offer a built-in <b>loopback input</b>.</li>
    <li>${mac ? 'In <b>Audio MIDI Setup</b> create a <b>Multi-Output Device</b> with your speakers/interface and BlackHole, so you keep hearing the mix.' : 'Route the DAW master to the virtual cable and monitor through it (or through VoiceMeeter) so you keep hearing the mix.'}</li>
    <li>Select that device as the DAW output${mac ? ' (the Multi-Output Device)' : ''}, then pick the driver here as the <b>Source</b>.</li>
    <li>Set the same sample rate in the DAW, the driver and here for a bit-transparent capture.</li></ol>
    <h4>WHERE THE OUTPUT DEVICE IS SET</h4><ul>${where}</ul>`;
  $('dawDlg').showModal();
}

// ───────────────────────────────────────────── wiring
async function init() {
  await refreshState();
  S.calibration = S.app.calibration;
  const p = S.app.prefs || {};
  if (p.sampleRate) $('rateSel').value = p.sampleRate;
  if (p.bitDepth) $('bitSel').value = p.bitDepth;
  S.rotation = Number(p.rotation) || 0;
  if (p.pipSize) S.pipSize = p.pipSize;
  if (p.picture === 'pip' || p.picture === 'split') S.layout = p.picture;
  S.screenCrop = validCrop(p.screenCrop);
  if (p.format === '9:16' || p.format === '16:9') S.format = p.format;
  if (Number(p.tallRatio)) S.tallRatio = Number(p.tallRatio);
  if (p.autoHide === false) $('autoHide').checked = false;
  if (p.splitSide) S.splitSide = p.splitSide;
  if (Number(p.splitRatio)) S.splitRatio = Number(p.splitRatio);
  if (p.pipSize != null) S.pipSize = p.pipSize;
  if (p.usbDevice) S.usb.deviceId = p.usbDevice;
  if (p.pipCorner) S.pipCorner = p.pipCorner;
  S.screenSource = p.screenSource || null;
  $('micOn').checked = !!p.mic;
  setMode(p.mode || 'studio');
  await listDevices();
  if (window.dlcNative && S.screenSource) sourceStillThere(S.screenSource).then((ok) => { if (!ok) S.screenSource = null; });
  navigator.mediaDevices.addEventListener('devicechange', () => { if (!engine.running) listDevices(); });
  connectWs();
  await listUsb().catch(() => {});
  if (p.camSource === 'usb') setCamSource('usb');
  initEditor();

  $('pairBtn').onclick = $('pairBtn2').onclick = openPair;
  $('unpairBtn').onclick = async () => { await api('/api/pair/reset', 'POST'); openPair(); };
  $('audioBtn').onclick = async () => {
    if (engine.running) { await engine.stop(); stopDisplay(); renderAudio(); } else connectAudio();
  };
  $('dawBtn').onclick = openDawGuide;
  $('micOn').onchange = renderAudio;
  $('recBtn').onclick = () => (S.rec ? stopRecording() : startRecording());
  $('pauseBtn').onclick = togglePause;
  $('modes').onclick = (e) => { const b = e.target.closest('button'); if (b && !b.disabled) setMode(b.dataset.mode, true); };
  for (const t of document.querySelectorAll('.tab')) t.onclick = () => showView(t.dataset.view);
  for (const b of document.querySelectorAll('[data-close]')) b.onclick = () => b.closest('dialog').close();

  segmented($('facingSeg'), (v) => camSet({ facing: v }));
  $('resSel').onchange = () => camSet({ height: Number($('resSel').value) });
  $('fpsSel').onchange = () => camSet({ fps: Number($('fpsSel').value) });
  $('zoom').oninput = () => { $('zoomOut').textContent = `${Number($('zoom').value).toFixed(1)}×`; camSetSoon({ zoom: Number($('zoom').value) }); };
  $('exposure').oninput = () => { camSetSoon({ exposure: Number($('exposure').value) }); };
  segmented($('focusSeg'), (v) => { $('focus').disabled = v !== 'manual'; camSet({ focusMode: v }); });
  $('focus').oninput = () => camSetSoon({ focusDistance: Number($('focus').value) });
  $('torchBtn').onclick = () => camSet({ torch: !(S.cam && S.cam.torch) });
  $('rotateBtn').onclick = () => { S.rotation = (S.rotation + 90) % 360; layoutPreview(); savePrefs(); };
  const savePrefsSoon = debounce(savePrefs, 400);
  $('camSizeStudio').oninput = () => {
    const v = Number($('camSizeStudio').value);
    if (S.layout === 'split') { if (S.format === '9:16') S.tallRatio = fromSlider('split', v); else S.splitRatio = fromSlider('split', v); }
    else S.pipSize = fromSlider('pip', v);
    layoutPreview(); savePrefsSoon();
  };
  dragDivider($('divStudio'), () => {
    const st = $('stage').getBoundingClientRect(), f = S.frame;
    return f ? { left: st.left + f.cx, width: f.cw, top: st.top + f.cy, height: f.ch, vertical: S.format === '9:16' } : null;
  }, () => S.splitSide === 'left', (r, vertical) => { if (vertical) S.tallRatio = r; else S.splitRatio = r; layoutPreview(); }, savePrefs);
  segmented($('formatStudio'), (v) => { S.format = v; layoutPreview(); savePrefs(); });
  $('mobileSplitBtn').onclick = () => mobileSplit();
  $('preview').addEventListener('wheel', (e) => {
    if (!composite() || S.layout !== 'pip') return;
    e.preventDefault();
    S.pipSize = Math.min(0.6, Math.max(0.15, pipFraction(S.pipSize) - Math.sign(e.deltaY) * 0.02));
    layoutPreview(); savePrefsSoon();
  }, { passive: false });
  segmented($('camSrcSeg'), (v) => setCamSource(v));
  $('autoHideRow').hidden = !window.dlcNative;
  $('autoHide').onchange = () => { savePrefs(); syncBusy(); };
  if (window.dlcNative) {
    window.dlcNative.onCommand(onShellCommand);
    setInterval(() => {
      if (!S.rec && !S.live) return;
      window.dlcNative.status({
        time: fmtClock(S.rec ? elapsed() : (S.live && S.live.seconds) || 0),
        recording: !!S.rec, paused: !!(S.rec && S.rec.pausedAt != null), live: !!(S.live && S.live.on),
      });
    }, 500);
  }
  S.liveCfg = S.app.live || {};
  segmented($('livePlat'), (v) => { saveLiveCfg(); loadLivePlatform(v); });
  for (const id of ['liveServer', 'liveKey', 'liveFormat', 'liveQual', 'liveVert']) $(id).addEventListener('change', () => { saveLiveCfg(); renderLive(); });
  $('liveServer').addEventListener('input', () => renderLive());
  $('liveBtn').onclick = () => (S.live ? stopLive() : goLive());
  $('liveScreenBtn').onclick = () => switchScreen();
  loadLivePlatform(S.liveCfg.platform || 'instagram');
  $('usbSel').onchange = () => { if (S.camSource === 'usb') startUsb(); };
  $('usbQuick').onclick = () => setCamSource('usb');
  navigator.mediaDevices.addEventListener('devicechange', () => listUsb().catch(() => {}));
  segmented($('pipCornerStudio'), (v) => { S.pipCorner = v; layoutPreview(); savePrefs(); });
  segmented($('layoutStudio'), (v) => { S.layout = v; layoutPreview(); savePrefs(); });
  $('igSplitBtn').onclick = () => instagramSplit();
  $('regionBtn').onclick = () => chooseRegion();
  $('regionFull').onclick = () => { S.screenCrop = null; layoutPreview(); savePrefs(); };
  $('screenMain').addEventListener('loadedmetadata', layoutPreview);
  $('screenMain').addEventListener('resize', layoutPreview);
  $('swapStudio').onclick = () => { S.splitSide = S.splitSide === 'left' ? 'right' : 'left'; layoutPreview(); savePrefs(); };
  dragToCorner($('preview'), () => (composite() && S.layout === 'pip' ? $('screenMain') : null), (c) => { S.pipCorner = c; layoutPreview(); savePrefs(); });
  $('screenOn').addEventListener('change', renderAudio);
  $('screenSrcBtn').onclick = async () => {
    try {
      if (engine.running) { await switchScreen(); return; }
      await pickScreenSource();
    } catch (e) { toast(e.message, 'error'); }
  };

  $('calBtn').onclick = () => {
    $('calText').textContent = 'Point the phone camera at this screen so it fills most of the picture, stop playback in your DAW, then start. The screen will flash a few times.';
    showCalibration();
    $('calDlg').showModal();
  };
  $('calStart').onclick = runCalibration;
  $('calClear').onclick = async () => { S.calibration = null; await api('/api/settings', 'PUT', { calibration: null }); showCalibration(); };

  $('preview').addEventListener('resize', layoutPreview);
  $('preview').addEventListener('loadedmetadata', () => { layoutPreview(); renderSteps(); });
  new ResizeObserver(layoutPreview).observe($('stage'));
  document.addEventListener('keydown', (e) => {
    if (e.target.closest('input, select, textarea, dialog') || $('view-studio').hidden) return;
    if (e.key === 'r' || e.key === 'R') { e.preventDefault(); $('recBtn').click(); }
    if (e.code === 'Space') { e.preventDefault(); if (!$('pauseBtn').disabled) togglePause(); }
  });
  window.addEventListener('beforeunload', (e) => { if (S.rec) { e.preventDefault(); e.returnValue = ''; } });

  const frame = () => {
    if (!$('view-studio').hidden) {
      drawMeters(); drawWave();
      $('timer').textContent = fmtClock(elapsed());
    }
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  setInterval(refreshState, 15000);
  renderRec();
}

init().catch((e) => toast(e.message, 'error'));
