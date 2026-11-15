// Builds the live picture (DAW screen + camera, same layout as the export)
// into one video track. Frames are pulled straight from the tracks and
// pushed by a timer, so it keeps running while the dashboard is minimised.
import { pipBox, splitBoxes } from './util.js';

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

function contain(ctx, img, box) {
  const s = Math.min(box.w / img.w, box.h / img.h);
  const w = img.w * s, h = img.h * s;
  ctx.drawImage(img.src, box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h);
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
    this.camera = null;
    this.timer = null;
  }

  setSources(screenTrack, cameraTrack) {
    const swap = (cur, track) => {
      if (cur && cur.origin === track) return cur;
      if (cur) cur.stop();
      if (!track || track.readyState !== 'live') return null;
      const fs = new FrameSource(track);
      fs.origin = track;
      return fs;
    };
    this.screen = swap(this.screen, screenTrack);
    this.camera = swap(this.camera, cameraTrack);
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
    const cam = this.camera && this.camera.image;
    if (scr && cam && L.layout === 'split') {
      const sb = splitBoxes(W, H, L.splitSide, L.splitRatio);
      contain(ctx, scr, sb.scr);
      cover(ctx, cam, sb.cam, L.rotation);
    } else if (scr && cam) {
      contain(ctx, scr, { x: 0, y: 0, w: W, h: H });
      const b = pipBox(W, H, L.pipSize, L.pipCorner);
      const o = Math.max(2, Math.round(b.size * 0.01));
      ctx.fillStyle = '#2ff3e0';
      ctx.fillRect(b.x, b.y, b.size, b.size);
      cover(ctx, cam, { x: b.x + o, y: b.y + o, w: b.size - 2 * o, h: b.size - 2 * o }, L.rotation);
    } else if (scr) {
      contain(ctx, scr, { x: 0, y: 0, w: W, h: H });
    } else if (cam) {
      cover(ctx, cam, { x: 0, y: 0, w: W, h: H }, L.rotation);
    }
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
    if (this.screen) this.screen.stop();
    if (this.camera) this.camera.stop();
    this.screen = this.camera = null;
  }
}
