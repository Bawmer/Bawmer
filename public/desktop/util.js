export const $ = (id) => document.getElementById(id);

export function toast(text, kind = '') {
  const el = document.createElement('div');
  el.className = `toast ${kind}`;
  el.textContent = text;
  $('toasts').append(el);
  setTimeout(() => el.remove(), kind === 'error' ? 7000 : 3500);
}

export async function api(path, method = 'GET', body) {
  const res = await fetch(path, {
    method, headers: body ? { 'Content-Type': 'application/json' } : undefined,
    body: body ? JSON.stringify(body) : undefined,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `Request failed (${res.status})`);
  return data;
}

export function fmtClock(sec) {
  const s = Math.max(0, Math.floor(sec));
  const p = (n) => String(n).padStart(2, '0');
  return `${p(Math.floor(s / 3600))}:${p(Math.floor(s / 60) % 60)}:${p(s % 60)}`;
}

export function fmtTime(sec) {
  const s = Math.max(0, sec);
  return `${Math.floor(s / 60)}:${(s % 60).toFixed(1).padStart(4, '0')}`;
}

export function fmtBytes(n) {
  if (n == null) return '—';
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n.toFixed(n >= 100 || i < 2 ? 0 : 1)} ${u[i]}`;
}

export const dB = (v) => (v > 0 ? 20 * Math.log10(v) : -Infinity);

/** Size a canvas for the device pixel ratio; returns its 2D context in CSS px. */
export function fitCanvas(canvas) {
  const r = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(r.width * dpr)), h = Math.max(1, Math.round(r.height * dpr));
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w: r.width, h: r.height };
}

export function segmented(el, onChange) {
  el.addEventListener('click', (e) => {
    const b = e.target.closest('button');
    if (!b || b.disabled) return;
    setSeg(el, b.dataset.v);
    onChange(b.dataset.v);
  });
}
export function setSeg(el, value) {
  for (const b of el.querySelectorAll('button')) b.classList.toggle('active', b.dataset.v === String(value));
}

export function debounce(fn, ms) {
  let t;
  return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); };
}

// Square camera box over the full DAW screen. Same maths as server/exporter.js
// (pipBox), so the previews show exactly what the export will contain.
const PIP_SIZES = { small: 0.28, medium: 0.36, large: 0.46 };
const even = (n) => Math.round(n / 2) * 2;
export function pipBox(w, h, size, corner) {
  const s = even(Math.min(w, h) * (PIP_SIZES[size] || PIP_SIZES.small));
  const m = even(Math.min(w, h) * 0.03);
  const c = ['tl', 'tr', 'bl', 'br'].includes(corner) ? corner : 'br';
  return { size: s, x: c[1] === 'l' ? m : w - s - m, y: c[0] === 't' ? m : h - s - m };
}

/** Nearest corner of a rectangle to a point. */
export function cornerAt(rect, x, y) {
  return (y < rect.top + rect.height / 2 ? 't' : 'b') + (x < rect.left + rect.width / 2 ? 'l' : 'r');
}

/** Let the user drag the camera box; it snaps to the nearest corner on release. */
export function dragToCorner(el, getArea, onCorner) {
  let start = null;
  el.addEventListener('pointerdown', (e) => {
    if (!getArea()) return;
    e.preventDefault();
    el.setPointerCapture(e.pointerId);
    start = { x: e.clientX, y: e.clientY, left: el.offsetLeft, top: el.offsetTop };
    el.classList.add('dragging');
  });
  el.addEventListener('pointermove', (e) => {
    if (!start) return;
    el.style.left = `${start.left + e.clientX - start.x}px`;
    el.style.top = `${start.top + e.clientY - start.y}px`;
  });
  const end = (e) => {
    if (!start) return;
    start = null;
    el.classList.remove('dragging');
    const area = getArea();
    if (area) onCorner(cornerAt(area.getBoundingClientRect(), e.clientX, e.clientY));
  };
  el.addEventListener('pointerup', end);
  el.addEventListener('pointercancel', end);
}
