// Session library and editor: preview, trim, sync adjustment and export.
import { $, toast, api, fmtTime, fmtBytes, fitCanvas, segmented, setSeg, debounce, pipBox, splitBoxes, frameBoxes, placeVideo, dragToCorner, pipFraction, toSlider, fromSlider, dragDivider } from './util.js';

let E = null; // the open session: { id, project, peaks, edit, dur }
let drag = null;
const MODE_NAMES = { studio: 'Studio Session', performance: 'Performance', tutorial: 'Tutorial', social: 'Social Media' };

const audio = () => $('edAudio');
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));

export async function refreshSessions() {
  const list = await api('/api/sessions');
  $('sessionsEmpty').hidden = list.length > 0;
  $('sessionList').innerHTML = '';
  for (const s of list) {
    const li = document.createElement('li');
    li.dataset.id = s.id;
    li.className = E && E.id === s.id ? 'active' : '';
    const b = document.createElement('b');
    b.textContent = s.title || 'Untitled session';
    const small = document.createElement('small');
    const tags = [s.hasCamera && 'cam', s.hasScreen && 'screen', s.audio && `${s.audio.sampleRate / 1000}k/${s.audio.bitDepth === 32 ? '32f' : '24'}`].filter(Boolean).join(' · ');
    small.textContent = s.status === 'ready'
      ? `${new Date(s.createdAt).toLocaleDateString()} · ${fmtTime(s.duration || 0)} · ${tags}`
      : 'Incomplete recording';
    li.append(b, small);
    if (s.status === 'ready') li.onclick = () => openSession(s.id);
    $('sessionList').append(li);
  }
  if (E && !list.some((s) => s.id === E.id)) closeEditor();
}

function closeEditor() {
  E = null;
  audio().pause();
  for (const id of ['edAudio', 'edCam', 'edCam2', 'edScreen']) { $(id).removeAttribute('src'); $(id).load(); }
  $('editor').hidden = true;
  $('editorEmpty').hidden = false;
}

export async function openSession(id) {
  const { project, peaks } = await api(`/api/sessions/${id}`);
  audio().pause();
  E = { id, project, peaks, edit: { ...project.edit }, dur: project.duration || 0 };
  $('editor').hidden = false;
  $('editorEmpty').hidden = true;
  for (const li of $('sessionList').children) li.classList.toggle('active', li.dataset.id === id);

  const media = (f) => `/media/${id}/${f}`;
  audio().src = media(project.audio.file);
  const cam = $('edCam'), scr = $('edScreen');
  if (project.camera) cam.src = media(project.camera.file); else { cam.removeAttribute('src'); cam.load(); }
  const cam2 = $('edCam2');
  if (project.camera2) cam2.src = media(project.camera2.file); else { cam2.removeAttribute('src'); cam2.load(); }
  $('edCam2Wrap').hidden = !project.camera2;
  if (project.screen) scr.src = media(project.screen.file); else { scr.removeAttribute('src'); scr.load(); }
  $('playerEmpty').hidden = !!(project.camera || project.camera2 || project.screen);

  $('edTitle').value = project.title || '';
  const a = project.audio;
  $('edMeta').textContent = `${MODE_NAMES[project.mode] || ''} · ${fmtTime(E.dur)} · ${a.sampleRate / 1000} kHz ${a.bitDepth === 32 ? '32-bit float' : '24-bit'} WAV`;
  $('layoutRow').hidden = !((project.camera || project.camera2) && project.screen);
  $('edMicRow').hidden = !project.mic;
  $('edRotate').hidden = !project.camera;
  const s = project.sync || {};
  const cal = project.timing && project.timing.calibration;
  $('syncInfo').textContent = project.camera
    ? `Aligned by timestamps${cal ? ' and your calibration' : ''}${s.driftCorrected ? `; clock drift corrected (${((s.cameraRate - 1) * 1e6).toFixed(0)} ppm)` : ''}. Positive values delay the picture.`
    : 'This session has no camera video.';
  $('originals').innerHTML = 'Originals: ' + [project.audio, project.mic, project.camera, project.camera2, project.screen].filter(Boolean)
    .map((f) => `<a href="${media(f.file)}?dl" download>${f.file}</a>`).join(' · ');
  renderEdit();
  renderExports();
  seek(E.edit.trimStart);
}

function renderEdit() {
  const e = E.edit;
  setSeg($('aspectSeg'), e.aspect);
  $('qualSel').value = e.quality;
  $('edFps').value = e.fps;
  $('layoutSel').value = e.layout;
  $('edMic').checked = !!e.includeMic;
  $('offset').value = e.manualOffsetMs;
  $('offOut').textContent = `${e.manualOffsetMs > 0 ? '+' : ''}${e.manualOffsetMs} ms`;
  $('edRotOut').textContent = `${e.rotation || 0}°`;
  const p = $('player'), pr = E.project;
  p.dataset.aspect = e.aspect;
  p.dataset.layout = !pr.screen ? 'camera' : !(pr.camera || pr.camera2) ? 'screen' : e.layout;
  const lay = p.dataset.layout;
  $('camSizeRow').hidden = lay !== 'pip' && lay !== 'split';
  $('cornerRow').hidden = lay !== 'pip';
  $('swapSides').hidden = lay !== 'split';
  $('divEditor').hidden = lay !== 'split';
  const ratio = Number(e.splitRatio) || 1 / 3;
  if (document.activeElement !== $('camSize')) $('camSize').value = lay === 'split' ? toSlider('split', ratio) : toSlider('pip', pipFraction(e.pipSize));
  $('camSizeOut').textContent = lay === 'split' ? `${Math.round(ratio * 100)}% of the width` : `${Math.round(pipFraction(e.pipSize) * 100)}% of the height`;
  setSeg($('pipCornerSeg'), e.pipCorner || 'br');
  layoutCam();
}

function layoutCam() {
  if (!E) return;
  const P = $('player'), scr = $('edScreen');
  const wraps = [document.querySelector('.ed-cam-wrap'), $('edCam2Wrap')];
  const vids = [$('edCam'), $('edCam2')];
  const pr = E.project, layout = P.dataset.layout, W = P.clientWidth, H = P.clientHeight;
  const place = (el, b) => Object.assign(el.style, { left: `${b.x}px`, top: `${b.y}px`, width: `${b.w}px`, height: `${b.h}px` });
  scr.style.left = scr.style.top = scr.style.width = scr.style.height = '';
  for (const w of wraps) w.style.left = w.style.top = w.style.width = w.style.height = '';
  // Same boxes as the export, so what you see is what you get.
  const present = [!!pr.camera, !!pr.camera2];
  const order = [0, 1].filter((i) => present[i]);
  const showCams = layout !== 'screen';
  const F = frameBoxes(W, H, { screen: layout !== 'camera' && !!pr.screen, layout, pipSize: E.edit.pipSize, pipCorner: E.edit.pipCorner,
    splitSide: E.edit.splitSide, splitRatio: E.edit.splitRatio }, showCams ? order.length : 0);
  if (F.scr && layout !== 'camera') place(scr, F.scr);
  order.forEach((ci, k) => {
    if (!showCams) return;
    place(wraps[ci], F.cams[k]);
    placeVideo(vids[ci], { x: 0, y: 0, w: F.cams[k].w, h: F.cams[k].h }, ci === 0 ? E.edit.rotation : 0);
  });
  if (layout === 'split' && F.scr && F.wide) Object.assign($('divEditor').style, { left: `${F.scr.x > 0 ? F.scr.x : F.scr.w}px`, top: '0px', height: `${H}px` });
  else $('divEditor').hidden = true;
}

const saveEdit = debounce(() => { if (E) api(`/api/sessions/${E.id}`, 'PATCH', { edit: E.edit }).catch(() => {}); }, 400);
function setEdit(patch) { Object.assign(E.edit, patch); renderEdit(); saveEdit(); }

// ── playback: the WAV is the master clock, videos chase it ──
function chase(v, target) {
  if (!v.getAttribute('src')) return;
  const inRange = target >= 0 && target <= (Number.isFinite(v.duration) ? v.duration : Infinity);
  v.classList.toggle('off', !inRange);
  if (!inRange) { if (!v.paused) v.pause(); return; }
  const drift = v.currentTime - target;
  if (audio().paused) {
    if (!v.paused) v.pause();
    if (Math.abs(drift) > 0.03 && !v.seeking) v.currentTime = target;
  } else {
    if (v.paused) v.play().catch(() => {});
    if (Math.abs(drift) > 0.15) { if (!v.seeking) v.currentTime = target; v.playbackRate = 1; }
    else v.playbackRate = clamp(1 - drift * 0.8, 0.9, 1.1);
  }
}

function tick() {
  if (E && !$('view-library').hidden) {
    const a = audio(), e = E.edit, s = E.project.sync || {};
    let t = a.currentTime;
    if (!a.paused) {
      const pause = ((E.project.timing && E.project.timing.pauses) || []).find(([p0, p1]) => t >= p0 && t < p1);
      if (pause) { a.currentTime = t = pause[1]; }
      if (t >= e.trimEnd) { a.pause(); a.currentTime = t = e.trimStart; }
    }
    chase($('edCam'), (t - (s.cameraOffsetSec || 0) - e.manualOffsetMs / 1000) / (s.cameraRate || 1));
    chase($('edCam2'), t - (s.camera2OffsetSec || 0) - e.manualOffsetMs / 1000);
    chase($('edScreen'), t - (s.screenOffsetSec || 0));
    $('edPlay').textContent = a.paused ? '▶' : '❚❚';
    $('edTime').textContent = `${fmtTime(t)} / ${fmtTime(E.dur)}  ·  selection ${fmtTime(e.trimEnd - e.trimStart)}`;
    drawTimeline(t);
  }
  requestAnimationFrame(tick);
}

function seek(t) {
  if (!E) return;
  audio().currentTime = clamp(t, 0, Math.max(0, E.dur - 0.01));
}

function drawTimeline(t) {
  const { ctx, w, h } = fitCanvas($('tlCanvas'));
  ctx.clearRect(0, 0, w, h);
  const { peaks, dur, edit } = E;
  const x = (sec) => (dur ? (sec / dur) * w : 0);
  const mid = h / 2;
  for (let px = 0; px < w; px += 2) {
    const i0 = Math.floor((px / w) * peaks.length), i1 = Math.max(i0 + 1, Math.floor(((px + 2) / w) * peaks.length));
    let m = 0;
    for (let i = i0; i < i1 && i < peaks.length; i++) if (peaks[i] > m) m = peaks[i];
    const sec = (px / w) * dur;
    ctx.fillStyle = sec >= edit.trimStart && sec <= edit.trimEnd ? '#2ff3e0' : '#39405a';
    const a = Math.max(0.5, Math.pow(m, 0.6) * (mid - 8));
    ctx.fillRect(px, mid - a, 1.3, a * 2);
  }
  ctx.fillStyle = '#ffb64833';
  for (const [p0, p1] of (E.project.timing && E.project.timing.pauses) || []) ctx.fillRect(x(p0), 0, Math.max(1, x(p1) - x(p0)), h);
  ctx.fillStyle = '#090a0fb0';
  ctx.fillRect(0, 0, x(edit.trimStart), h);
  ctx.fillRect(x(edit.trimEnd), 0, w - x(edit.trimEnd), h);
  for (const sec of [edit.trimStart, edit.trimEnd]) {
    ctx.fillStyle = '#a66bff';
    ctx.fillRect(clamp(x(sec) - 1.5, 0, w - 3), 0, 3, h);
    ctx.fillRect(clamp(x(sec) - 4, 0, w - 8), mid - 12, 8, 24);
  }
  ctx.fillStyle = '#fff';
  ctx.fillRect(clamp(x(t), 0, w - 1), 0, 1.5, h);
}

function timelinePointer(ev) {
  const r = $('timeline').getBoundingClientRect();
  const sec = clamp(((ev.clientX - r.left) / r.width) * E.dur, 0, E.dur);
  if (ev.type === 'pointerdown') {
    const px = (s) => (s / E.dur) * r.width + r.left;
    const dIn = Math.abs(ev.clientX - px(E.edit.trimStart)), dOut = Math.abs(ev.clientX - px(E.edit.trimEnd));
    drag = Math.min(dIn, dOut) <= 9 ? (dIn <= dOut ? 'in' : 'out') : 'seek';
    $('timeline').setPointerCapture(ev.pointerId);
  }
  if (!drag) return;
  if (drag === 'in') setEdit({ trimStart: Math.min(sec, E.edit.trimEnd - 0.2) });
  else if (drag === 'out') setEdit({ trimEnd: Math.max(sec, E.edit.trimStart + 0.2) });
  else seek(sec);
}

// ── exports ──
function renderExports() {
  const ul = $('exportList');
  ul.innerHTML = '';
  for (const x of [...(E.project.exports || [])].reverse()) {
    const li = document.createElement('li');
    const name = document.createElement('span');
    name.textContent = x.file.replace('exports/', '');
    const link = document.createElement('a');
    link.href = `/media/${E.id}/${x.file}?dl`;
    link.textContent = fmtBytes(x.size);
    link.title = 'Save a copy';
    li.append(name, link);
    ul.append(li);
  }
}

async function startExport(kind) {
  if (!E) return;
  try {
    setBusy(true, 'Starting…', 0);
    await api(`/api/sessions/${E.id}/export`, 'POST', { kind, edit: E.edit });
  } catch (e) {
    setBusy(false);
    toast(e.message, 'error');
  }
}

function setBusy(busy, text, p) {
  $('expMp4').disabled = $('expWav').disabled = busy;
  $('expProgress').hidden = !busy;
  if (busy) {
    $('expProgress').firstElementChild.style.width = `${Math.round(p * 100)}%`;
    $('expProgress').lastElementChild.textContent = text;
  }
}

export async function onExportMessage(msg) {
  const mine = E && E.id === msg.id;
  if (msg.type === 'export.progress') { if (mine) setBusy(true, `${msg.name} — ${Math.round(msg.progress * 100)}%`, msg.progress); return; }
  if (mine) setBusy(false);
  if (msg.type === 'export.error') return toast(`Export failed: ${msg.message}`, 'error');
  toast(`Exported ${msg.name}`, 'ok');
  const done = document.querySelector('#steps [data-step="export"]');
  if (done) done.className = 'done';
  if (mine) { E.project = (await api(`/api/sessions/${E.id}`)).project; renderExports(); }
}

export function initEditor() {
  $('edPlay').onclick = () => {
    const a = audio();
    if (!E) return;
    if (a.paused) {
      if (a.currentTime < E.edit.trimStart || a.currentTime >= E.edit.trimEnd - 0.05) a.currentTime = E.edit.trimStart;
      a.play().catch((e) => toast(e.message, 'error'));
    } else a.pause();
  };
  $('setIn').onclick = () => setEdit({ trimStart: Math.min(audio().currentTime, E.edit.trimEnd - 0.2) });
  $('setOut').onclick = () => setEdit({ trimEnd: Math.max(audio().currentTime, E.edit.trimStart + 0.2) });
  $('resetTrim').onclick = () => setEdit({ trimStart: 0, trimEnd: E.dur });
  const tl = $('timeline');
  tl.addEventListener('pointerdown', (e) => E && timelinePointer(e));
  tl.addEventListener('pointermove', (e) => E && drag && timelinePointer(e));
  tl.addEventListener('pointerup', () => { drag = null; });
  tl.addEventListener('pointercancel', () => { drag = null; });

  $('offset').oninput = () => setEdit({ manualOffsetMs: Number($('offset').value) });
  document.querySelector('.nudge').onclick = (e) => {
    const b = e.target.closest('button');
    if (!b) return;
    const n = Number(b.dataset.n);
    setEdit({ manualOffsetMs: n === 0 ? 0 : clamp(E.edit.manualOffsetMs + n, -500, 500) });
  };
  segmented($('aspectSeg'), (v) => setEdit({ aspect: v }));
  $('qualSel').onchange = () => setEdit({ quality: Number($('qualSel').value) });
  $('edFps').onchange = () => setEdit({ fps: Number($('edFps').value) });
  $('layoutSel').onchange = () => setEdit({ layout: $('layoutSel').value });
  $('camSize').oninput = () => {
    const v = Number($('camSize').value);
    setEdit($('player').dataset.layout === 'split' ? { splitRatio: fromSlider('split', v) } : { pipSize: fromSlider('pip', v) });
  };
  dragDivider($('divEditor'), () => (E ? $('player').getBoundingClientRect() : null),
    () => E.edit.splitSide === 'left', (r) => setEdit({ splitRatio: r }));
  $('swapSides').onclick = () => setEdit({ splitSide: E.edit.splitSide === 'left' ? 'right' : 'left' });
  segmented($('pipCornerSeg'), (v) => setEdit({ pipCorner: v }));
  dragToCorner(document.querySelector('.ed-cam-wrap'),
    () => (E && $('player').dataset.layout === 'pip' ? $('player') : null),
    (c) => setEdit({ pipCorner: c }));
  $('edMic').onchange = () => setEdit({ includeMic: $('edMic').checked });
  $('edRotate').onclick = () => setEdit({ rotation: ((E.edit.rotation || 0) + 90) % 360 });
  $('expMp4').onclick = () => startExport('mp4');
  $('expWav').onclick = () => startExport('wav');
  $('reveal').onclick = async () => {
    const r = await api(`/api/sessions/${E.id}/reveal`, 'POST').catch((e) => toast(e.message, 'error'));
    if (r) toast(r.path);
  };
  $('edTitle').onchange = async () => {
    await api(`/api/sessions/${E.id}`, 'PATCH', { title: $('edTitle').value });
    E.project.title = $('edTitle').value;
    refreshSessions();
  };
  $('edDelete').onclick = async () => {
    if (!E || !confirm(`Delete “${E.project.title}” and all of its files? This cannot be undone.`)) return;
    try { await api(`/api/sessions/${E.id}`, 'DELETE'); closeEditor(); refreshSessions(); } catch (e) { toast(e.message, 'error'); }
  };
  $('edCam').addEventListener('loadedmetadata', layoutCam);
  $('edCam2').addEventListener('loadedmetadata', layoutCam);
  new ResizeObserver(layoutCam).observe($('player'));
  document.addEventListener('keydown', (e) => {
    if ($('view-library').hidden || !E || e.target.closest('input, select, textarea, dialog')) return;
    if (e.code === 'Space') { e.preventDefault(); $('edPlay').click(); }
  });
  requestAnimationFrame(tick);
}
