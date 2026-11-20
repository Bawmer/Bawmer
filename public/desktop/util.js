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
/** Side by side: complete DAW in one panel, camera in the other (server/exporter.js splitBoxes). */
const clampN = (v, a, b) => Math.min(b, Math.max(a, v));
export function splitBoxes(w, h, side, ratio, tallRatio) {
  const camFirst = side === 'left';
  if (w > h) {
    const r = clampN(Number(ratio) || 1 / 3, 0.2, 0.8); // camera's share of the width
    const cw = even(w * r), sw = w - cw;
    return { scr: { x: camFirst ? cw : 0, y: 0, w: sw, h }, cam: { x: camFirst ? 0 : sw, y: 0, w: cw, h }, wide: true, camFirst };
  }
  // Tall frame (9:16): camera above/below the DAW. With tallRatio the camera
  // takes that share of the height; otherwise the DAW gets a full-width 16:9 band.
  const tr = Number(tallRatio);
  const sh = tr > 0 ? h - even(h * clampN(tr, 0.2, 0.8)) : Math.min(even((w * 9) / 16), even(h / 2)), ch = h - sh;
  return { scr: { x: 0, y: camFirst ? ch : 0, w, h: sh }, cam: { x: 0, y: camFirst ? 0 : sh, w, h: ch }, wide: false, camFirst };
}

/** Position a (possibly 90°-rotated) video so it exactly covers a box. */
export function placeVideo(el, box, rotation, offsetX = 0, offsetY = 0) {
  const rot = (rotation || 0) % 180 !== 0;
  const ew = rot ? box.h : box.w, eh = rot ? box.w : box.h;
  Object.assign(el.style, {
    left: `${offsetX + box.x + box.w / 2 - ew / 2}px`, top: `${offsetY + box.y + box.h / 2 - eh / 2}px`,
    width: `${ew}px`, height: `${eh}px`, transform: `rotate(${rotation || 0}deg)`,
  });
}

export function pipBox(w, h, size, corner) {
  const f = typeof size === 'number' ? clampN(size, 0.15, 0.6) : PIP_SIZES[size] || PIP_SIZES.small;
  const s = even(Math.min(w, h) * f);
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

// One "camera size" slider (0–100) drives whichever layout is active.
export const SIZE = {
  split: { min: 0.2, max: 0.8 }, // camera's share of the width
  pip: { min: 0.15, max: 0.6 }, // box side as a share of the frame height
};
export const pipFraction = (size) => (typeof size === 'number' ? size : PIP_SIZES[size] || PIP_SIZES.small);
export const toSlider = (layout, value) => Math.round(((value - SIZE[layout].min) / (SIZE[layout].max - SIZE[layout].min)) * 100);
export const fromSlider = (layout, v) => SIZE[layout].min + (SIZE[layout].max - SIZE[layout].min) * (v / 100);

/** Drag a divider along x; reports the camera's share of the frame width. */
export function dragDivider(handle, getFrame, isCamFirst, onRatio, onEnd) {
  let on = false;
  handle.addEventListener('pointerdown', (e) => { on = true; handle.setPointerCapture(e.pointerId); e.preventDefault(); });
  handle.addEventListener('pointermove', (e) => {
    if (!on) return;
    const f = getFrame();
    if (!f) return;
    // Wide frames split left/right; tall (9:16) frames split top/bottom.
    const p = f.vertical ? (e.clientY - f.top) / f.height : (e.clientX - f.left) / f.width;
    onRatio(clampN(isCamFirst() ? p : 1 - p, 0.2, 0.8), !!f.vertical);
  });
  const end = () => { if (on) { on = false; if (onEnd) onEnd(); } };
  handle.addEventListener('pointerup', end);
  handle.addEventListener('pointercancel', end);
}

/** DAW screen + 0–2 cameras in a frame (same as server/exporter.js frameBoxes). */
export function frameBoxes(w, h, o, n) {
  const full = { x: 0, y: 0, w, h };
  const halves = (c, sideBySide) => {
    if (sideBySide) { const a = even(c.w / 2); return [{ x: c.x, y: c.y, w: a, h: c.h }, { x: c.x + a, y: c.y, w: c.w - a, h: c.h }]; }
    const a = even(c.h / 2);
    return [{ x: c.x, y: c.y, w: c.w, h: a }, { x: c.x, y: c.y + a, w: c.w, h: c.h - a }];
  };
  if (!o.screen) return { scr: null, cams: n === 0 ? [] : n === 1 ? [full] : halves(full, w >= h) };
  if (n === 0) return { scr: full, cams: [] };
  if (o.layout === 'split') {
    const sb = splitBoxes(w, h, o.splitSide, o.splitRatio, o.tallRatio);
    return { scr: sb.scr, cams: n === 1 ? [sb.cam] : halves(sb.cam, !sb.wide), wide: sb.wide, camFirst: sb.camFirst };
  }
  const corner = ['tl', 'tr', 'bl', 'br'].includes(o.pipCorner) ? o.pipCorner : 'br';
  const corners = [corner, corner[0] + (corner[1] === 'l' ? 'r' : 'l')];
  const cams = corners.slice(0, n).map((c) => { const b = pipBox(w, h, o.pipSize, c); return { x: b.x, y: b.y, w: b.size, h: b.size, outline: true }; });
  return { scr: full, cams };
}

/** A usable DAW region ({x, y, w, h} as fractions of the screen) or null for the whole screen. */
export function validCrop(c) {
  if (!c || typeof c !== 'object') return null;
  const n = (v) => Math.min(1, Math.max(0, Number(v) || 0));
  const x = n(c.x), y = n(c.y), w = Math.min(n(c.w), 1 - x), h = Math.min(n(c.h), 1 - y);
  if (w < 0.03 || h < 0.03 || (w > 0.999 && h > 0.999)) return null;
  return { x, y, w, h };
}

/**
 * Show a screen <video> in a box, cropped to a DAW region and fitted inside
 * the box (never cut off): the element is sized for the whole picture and a
 * clip-path leaves only the region visible.
 */
export function placeScreen(el, box, crop, offX = 0, offY = 0) {
  const c = validCrop(crop);
  const vw = el.videoWidth, vh = el.videoHeight;
  if (!c || !vw || !vh) {
    Object.assign(el.style, { left: `${offX + box.x}px`, top: `${offY + box.y}px`, width: `${box.w}px`, height: `${box.h}px`, objectFit: 'contain', clipPath: '' });
    return;
  }
  const rw = c.w * vw, rh = c.h * vh;
  const s = Math.min(box.w / rw, box.h / rh);
  const ew = vw * s, eh = vh * s;
  const rx = offX + box.x + (box.w - rw * s) / 2, ry = offY + box.y + (box.h - rh * s) / 2;
  Object.assign(el.style, {
    left: `${rx - c.x * ew}px`, top: `${ry - c.y * eh}px`, width: `${ew}px`, height: `${eh}px`, objectFit: 'fill',
    clipPath: `inset(${c.y * eh}px ${(1 - c.x - c.w) * ew}px ${(1 - c.y - c.h) * eh}px ${c.x * ew}px)`,
  });
}
