// "Choose the part of the DAW to show": a live view of the captured screen
// with a frame you drag over the timeline. The frame keeps the shape of the
// panel it will fill, so the chosen part fills that panel exactly.
import { validCrop } from './util.js';

let dlg = null;

function build() {
  dlg = document.createElement('dialog');
  dlg.className = 'dlg wide region-dlg';
  dlg.innerHTML = `
    <h2>Choose the part of the DAW to show</h2>
    <p>Drag the frame over your <b>timeline</b> (or any part of the DAW). Drag the corner dot to resize. The frame has the shape of the DAW panel, so what is inside fills it exactly.</p>
    <div class="region-stage"><canvas></canvas><div class="region-sel"><i class="region-handle"></i></div></div>
    <div class="dlg-actions">
      <button class="btn ghost" data-act="full">Show the whole DAW</button>
      <span class="spacer"></span>
      <button class="btn" data-act="cancel">Cancel</button>
      <button class="btn primary" data-act="ok">Use this part</button>
    </div>`;
  document.body.append(dlg);
}

/**
 * @param source  () => { src, w, h } | null   current picture of the screen
 * @param aspect  width/height of the panel the region will fill
 * @param initial the current region, or null
 * @returns Promise<{x,y,w,h} | null | undefined>  region, null = whole DAW, undefined = cancelled
 */
export function selectRegion({ source, aspect, initial }) {
  if (!dlg) build();
  const canvas = dlg.querySelector('canvas');
  const stage = dlg.querySelector('.region-stage');
  const sel = dlg.querySelector('.region-sel');
  const ctx = canvas.getContext('2d');
  const first = source();
  if (!first) return Promise.resolve(undefined);

  // Display size: fit the screen picture into the dialog.
  const maxW = Math.min(900, window.innerWidth * 0.86), maxH = window.innerHeight * 0.58;
  const k = Math.min(maxW / first.w, maxH / first.h);
  const W = Math.round(first.w * k), H = Math.round(first.h * k);
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(W * dpr); canvas.height = Math.round(H * dpr);
  Object.assign(canvas.style, { width: `${W}px`, height: `${H}px` });
  Object.assign(stage.style, { width: `${W}px`, height: `${H}px` });

  // Frame in display pixels, with the panel's shape.
  const A = aspect > 0 ? aspect : 16 / 9;
  const init = validCrop(initial);
  let r;
  if (init) {
    r = { x: init.x * W, y: init.y * H, w: init.w * W, h: init.w * W / A };
  } else {
    const w = Math.min(W * 0.5, H * 0.8 * A);
    r = { x: (W - w) / 2, y: (H - w / A) / 2, w, h: w / A };
  }
  const clampR = () => {
    r.w = Math.max(30, Math.min(r.w, W, H * A));
    r.h = r.w / A;
    r.x = Math.max(0, Math.min(r.x, W - r.w));
    r.y = Math.max(0, Math.min(r.y, H - r.h));
    Object.assign(sel.style, { left: `${r.x}px`, top: `${r.y}px`, width: `${r.w}px`, height: `${r.h}px` });
  };
  clampR();

  const draw = () => {
    const img = source();
    if (img) ctx.drawImage(img.src, 0, 0, canvas.width, canvas.height);
  };
  draw();
  const timer = setInterval(draw, 400); // live, so you can see the timeline moving

  let drag = null;
  sel.onpointerdown = (e) => {
    e.preventDefault();
    sel.setPointerCapture(e.pointerId);
    drag = { mode: e.target.classList.contains('region-handle') ? 'size' : 'move', x: e.clientX, y: e.clientY, r: { ...r } };
  };
  sel.onpointermove = (e) => {
    if (!drag) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    if (drag.mode === 'move') { r.x = drag.r.x + dx; r.y = drag.r.y + dy; }
    else { r.w = Math.max(drag.r.w + dx, (drag.r.h + dy) * A); r.x = drag.r.x; r.y = drag.r.y; }
    clampR();
  };
  sel.onpointerup = sel.onpointercancel = () => { drag = null; };

  return new Promise((resolve) => {
    const done = (value) => { clearInterval(timer); dlg.close(); resolve(value); };
    dlg.querySelector('[data-act="ok"]').onclick = () => done({ x: r.x / W, y: r.y / H, w: r.w / W, h: r.h / H });
    dlg.querySelector('[data-act="full"]').onclick = () => done(null);
    dlg.querySelector('[data-act="cancel"]').onclick = () => done(undefined);
    dlg.oncancel = (e) => { e.preventDefault(); done(undefined); };
    dlg.showModal();
  });
}
