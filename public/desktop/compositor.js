// Builds the live picture (DAW screen + camera, same layout as the export)
// into one video track. Frames are pulled straight from the tracks and
// pushed by a timer, so it keeps running while the dashboard is minimised.
import { frameBoxes, validCrop } from './util.js';

const hasProcessor = typeof MediaStreamTrackProcessor !== 'undefined';
const hasGenerator = typeof MediaStreamTrackGenerator !== 'undefined';

/** Latest frame of a track, via MediaStreamTrackProcessor or a hidden <video>. */
class FrameSource {
  constructor(track) {
    this.track = track.clone();
    this.frame = null;
    this.on = true;
    if (hasProcessor) {
      this.reader = new MediaStreamTrackProcessor({ track: this.track }).readable.getReader();
      (async () => {
        while (this.on) {
          const { value, done } = await this.reader.read().catch(() => ({ done: true }));
          if (done) break;
          if (!this.on) { value.close(); break; }
          if (this.frame) this.frame.close();
          this.frame = value;
        }
      })();
    } else {
      this.video = document.createElement('video');
      this.video.muted = true;
      this.video.playsInline = true;
      this.video.srcObject = new MediaStream([this.track]);
      this.video.play().catch(() => {});
    }
  }
  get image() {
    if (this.frame) return { src: this.frame, w: this.frame.displayWidth, h: this.frame.displayHeight };
    if (this.video && this.video.videoWidth) return { src: this.video, w: this.video.videoWidth, h: this.video.videoHeight };
    return null;
  }
  stop() {
    this.on = false;
    if (this.reader) this.reader.cancel().catch(() => {});
    if (this.frame) { this.frame.close(); this.frame = null; }
    if (this.video) this.video.srcObject = null;
    this.track.stop();
  }
}

/** Fit a picture (or the region { x, y, w, h } of it, as fractions) inside a box. */
function contain(ctx, img, box, crop) {
  const c = validCrop(crop);
  const sx = c ? c.x * img.w : 0, sy = c ? c.y * img.h : 0;
  const sw = c ? c.w * img.w : img.w, sh = c ? c.h * img.h : img.h;
  const s = Math.min(box.w / sw, box.h / sh);
  const w = sw * s, h = sh * s;
  ctx.drawImage(img.src, sx, sy, sw, sh, box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h);
}

function cover(ctx, img, box, rotation) {
  const rot = ((rotation || 0) % 360 + 360) % 360;
  const turned = rot % 180 !== 0;
  const bw = turned ? box.h : box.w, bh = turned ? box.w : box.h;
  const s = Math.max(bw / img.w, bh / img.h);
  ctx.save();
  ctx.beginPath();
  ctx.rect(box.x, box.y, box.w, box.h);
  ctx.clip();
  ctx.translate(box.x + box.w / 2, box.y + box.h / 2);
  ctx.rotate((rot * Math.PI) / 180);
  ctx.drawImage(img.src, (-img.w * s) / 2, (-img.h * s) / 2, img.w * s, img.h * s);
  ctx.restore();
}

export class Compositor {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d', { alpha: false });
    this.screen = null;
    this.cameras = [];
    this.timer = null;
  }

  /** Screen track plus up to two camera tracks (phone, USB). */
  setSources(screenTrack, cameraTracks) {
    const swap = (cur, track) => {
      if (cur && cur.origin === track) return cur;
      if (cur) cur.stop();
      if (!track || track.readyState !== 'live') return null;
      const fs = new FrameSource(track);
      fs.origin = track;
      return fs;
    };
    // A ScreenRelay is drawn straight from its canvas: no second decode of the same picture.
    if (screenTrack instanceof ScreenRelay) {
      if (this.screen && this.screen.stop && !this.screen.relay) this.screen.stop();
      const relay = screenTrack;
      this.screen = { relay, origin: relay, get image() { return relay.live ? { src: relay.canvas, w: relay.width, h: relay.height } : null; } };
    } else {
      if (this.screen && this.screen.relay) this.screen = null;
      this.screen = swap(this.screen, screenTrack);
    }
    const list = (cameraTracks || []).filter(Boolean);
    const next = [];
    for (let i = 0; i < 2; i++) next.push(swap(this.cameras[i] || null, list[i] || null));
    this.cameras = next;
  }

  /** @param getLayout () => { layout, pipSize, pipCorner, splitSide, splitRatio, rotation } */
  start({ width, height, fps, getLayout }) {
    this.stop();
    this.canvas.width = width;
    this.canvas.height = height;
    this.getLayout = getLayout;
    if (hasGenerator) {
      this.generator = new MediaStreamTrackGenerator({ kind: 'video' });
      this.writer = this.generator.writable.getWriter();
      this.track = this.generator;
    } else {
      this.track = this.canvas.captureStream(fps).getVideoTracks()[0];
    }
    const t0 = performance.now();
    this.timer = setInterval(() => {
      this.draw();
      if (this.writer) {
        const frame = new VideoFrame(this.canvas, { timestamp: Math.round((performance.now() - t0) * 1000) });
        this.writer.write(frame).catch(() => {}).finally(() => frame.close());
      }
    }, 1000 / fps);
    return this.track;
  }

  draw() {
    const { ctx } = this;
    const W = this.canvas.width, H = this.canvas.height;
    const L = this.getLayout();
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, W, H);
    const scr = this.screen && this.screen.image;
    const cams = this.cameras.map((c, i) => (c && c.image ? { img: c.image, rot: i === 0 ? L.rotation : 0 } : null)).filter(Boolean);
    const F = frameBoxes(W, H, { ...L, screen: !!scr }, cams.length);
    if (F.scr) contain(ctx, scr, F.scr, L.screenCrop);
    F.cams.forEach((box, i) => {
      if (box.outline) {
        const o = Math.max(2, Math.round(box.w * 0.01));
        ctx.fillStyle = '#2ff3e0';
        ctx.fillRect(box.x, box.y, box.w, box.h);
        cover(ctx, cams[i].img, { x: box.x + o, y: box.y + o, w: box.w - 2 * o, h: box.h - 2 * o }, cams[i].rot);
      } else cover(ctx, cams[i].img, box, cams[i].rot);
    });
  }

  stop() {
    clearInterval(this.timer);
    this.timer = null;
    if (this.writer) { this.writer.close().catch(() => {}); this.writer = null; }
    if (this.track) { this.track.stop(); this.track = null; }
    this.generator = null;
  }

  dispose() {
    this.stop();
    if (this.screen && this.screen.stop) this.screen.stop();
    for (const c of this.cameras) if (c) c.stop();
    this.screen = null;
    this.cameras = [];
  }
}

/**
 * A steady screen track whose source can be swapped while recording or live:
 * the recorder, the preview and the live picture all read this one track, so
 * picking another screen or window never interrupts them. If the captured
 * window closes, the last picture is held until a new source is chosen.
 */
export class ScreenRelay {
  constructor(track, fps = 30) {
    const s = track.getSettings();
    this.width = Math.max(2, Math.round((s.width || 1920) / 2) * 2);
    this.height = Math.max(2, Math.round((s.height || 1080) / 2) * 2);
    this.canvas = document.createElement('canvas');
    this.canvas.width = this.width;
    this.canvas.height = this.height;
    this.ctx = this.canvas.getContext('2d', { alpha: false });
    this.ctx.fillStyle = '#000';
    this.ctx.fillRect(0, 0, this.width, this.height);
    this.source = null;
    this.onSourceEnded = null;
    this.switchTo(track);
    if (hasGenerator) {
      this.generator = new MediaStreamTrackGenerator({ kind: 'video' });
      this.writer = this.generator.writable.getWriter();
      this.track = this.generator;
    } else {
      this.track = this.canvas.captureStream(fps).getVideoTracks()[0];
    }
    this.track.contentHint = 'detail'; // keep small DAW text sharp
    const t0 = performance.now();
    this.timer = setInterval(() => {
      const img = this.source && this.source.image;
      if (img) {
        this.ctx.fillStyle = '#000';
        this.ctx.fillRect(0, 0, this.width, this.height);
        contain(this.ctx, img, { x: 0, y: 0, w: this.width, h: this.height });
      }
      if (this.writer) {
        const frame = new VideoFrame(this.canvas, { timestamp: Math.round((performance.now() - t0) * 1000) });
        this.writer.write(frame).catch(() => {}).finally(() => frame.close());
      }
    }, 1000 / fps);
  }

  /** Use another screen or window from now on. */
  switchTo(track) {
    const old = this.source;
    this.source = new FrameSource(track);
    this.source.origin = track;
    track.addEventListener('ended', () => { if (this.source && this.source.origin === track && this.onSourceEnded) this.onSourceEnded(); });
    if (old) { old.stop(); old.origin.stop(); }
  }

  get live() { return !!this.track && this.track.readyState === 'live'; }

  stop() {
    clearInterval(this.timer);
    if (this.writer) { this.writer.close().catch(() => {}); this.writer = null; }
    if (this.track) this.track.stop();
    if (this.source) { this.source.stop(); this.source.origin.stop(); }
    this.source = null;
  }
}
