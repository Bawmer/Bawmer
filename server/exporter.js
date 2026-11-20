'use strict';
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const { keepSegments } = require('./sync');

function ffmpegPath() {
  if (process.env.FFMPEG_PATH) return process.env.FFMPEG_PATH;
  try {
    const p = require('ffmpeg-static');
    if (p) {
      const unpacked = p.replace('app.asar' + path.sep, 'app.asar.unpacked' + path.sep);
      if (fs.existsSync(unpacked)) return unpacked;
    }
  } catch { /* fall through */ }
  return 'ffmpeg';
}

const even = (n) => Math.round(n / 2) * 2;

// Square camera box over the full DAW screen. Keep in sync with
// public/desktop/util.js (pipBox), which draws the same box in the previews.
const PIP_SIZES = { small: 0.28, medium: 0.36, large: 0.46 };
// Side by side: the complete DAW in one panel, the camera in the other.
// Wide canvases put the camera in a third of the width (left or right);
// tall/square canvases stack them, with a full-width 16:9 DAW panel.
// Keep in sync with public/desktop/util.js (splitBoxes).
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
function splitBoxes(w, h, side, ratio) {
  const camFirst = side === 'left';
  if (w > h) {
    const r = clamp(Number(ratio) || 1 / 3, 0.2, 0.8); // camera's share of the width
    const cw = even(w * r), sw = w - cw;
    return { scr: { x: camFirst ? cw : 0, y: 0, w: sw, h }, cam: { x: camFirst ? 0 : sw, y: 0, w: cw, h }, wide: true, camFirst };
  }
  const sh = Math.min(even((w * 9) / 16), even(h / 2)), ch = h - sh;
  return { scr: { x: 0, y: camFirst ? ch : 0, w, h: sh }, cam: { x: 0, y: camFirst ? 0 : sh, w, h: ch }, wide: false, camFirst };
}

function pipBox(w, h, size, corner) {
  const f = typeof size === 'number' ? clamp(size, 0.15, 0.6) : PIP_SIZES[size] || PIP_SIZES.small;
  const s = even(Math.min(w, h) * f);
  const m = even(Math.min(w, h) * 0.03);
  const c = ['tl', 'tr', 'bl', 'br'].includes(corner) ? corner : 'br';
  return { size: s, x: c[1] === 'l' ? m : w - s - m, y: c[0] === 't' ? m : h - s - m };
}
const f = (n) => Number(n).toFixed(6);

function canvasSize(aspect, quality) {
  const q = [720, 1080, 1440, 2160].includes(+quality) ? +quality : 1080;
  if (aspect === '9:16') return { w: q, h: even((q * 16) / 9) };
  if (aspect === '1:1') return { w: q, h: q };
  return { w: even((q * 16) / 9), h: q };
}

/**
 * Where everything goes in a frame: the DAW screen (fitted, never cropped)
 * and zero, one or two cameras (filled). Keep in sync with
 * public/desktop/util.js (frameBoxes).
 */
function frameBoxes(w, h, o, n) {
  const full = { x: 0, y: 0, w, h };
  const halves = (c, sideBySide) => {
    if (sideBySide) { const a = even(c.w / 2); return [{ x: c.x, y: c.y, w: a, h: c.h }, { x: c.x + a, y: c.y, w: c.w - a, h: c.h }]; }
    const a = even(c.h / 2);
    return [{ x: c.x, y: c.y, w: c.w, h: a }, { x: c.x, y: c.y + a, w: c.w, h: c.h - a }];
  };
  if (!o.screen) return { scr: null, cams: n === 0 ? [] : n === 1 ? [full] : halves(full, w >= h) };
  if (n === 0) return { scr: full, cams: [] };
  if (o.layout === 'split') {
    const sb = splitBoxes(w, h, o.splitSide, o.splitRatio);
    return { scr: sb.scr, cams: n === 1 ? [sb.cam] : halves(sb.cam, !sb.wide), wide: sb.wide };
  }
  const corner = ['tl', 'tr', 'bl', 'br'].includes(o.pipCorner) ? o.pipCorner : 'br';
  const corners = [corner, corner[0] + (corner[1] === 'l' ? 'r' : 'l')]; // second camera: mirrored corner
  const cams = corners.slice(0, n).map((c) => { const b = pipBox(w, h, o.pipSize, c); return { x: b.x, y: b.y, w: b.size, h: b.size, outline: true }; });
  return { scr: full, cams };
}

/**
 * Part of the DAW screen to show, as fractions of the full screen
 * ({ x, y, w, h }), or null for the whole screen. The recording always keeps
 * the full screen; the crop is applied here.
 */
function validCrop(c) {
  if (!c || typeof c !== 'object') return null;
  const n = (v) => Math.min(1, Math.max(0, Number(v) || 0));
  const x = n(c.x), y = n(c.y), w = Math.min(n(c.w), 1 - x), h = Math.min(n(c.h), 1 - y);
  if (w < 0.03 || h < 0.03 || (w > 0.999 && h > 0.999)) return null;
  return { x, y, w, h };
}
function cropFilter(c) {
  const k = validCrop(c);
  if (!k) return '';
  const f6 = (v) => v.toFixed(6);
  return `crop=trunc(iw*${f6(k.w)}/2)*2:trunc(ih*${f6(k.h)}/2)*2:trunc(iw*${f6(k.x)}):trunc(ih*${f6(k.y)}),`;
}

function rotateFilter(deg) {
  const d = ((Number(deg) || 0) % 360 + 360) % 360;
  if (d === 90) return 'transpose=1,';
  if (d === 180) return 'hflip,vflip,';
  if (d === 270) return 'transpose=2,';
  return '';
}

/** Build the ffmpeg argument list for an MP4 export. Pure, so it is testable. */
function buildMp4Args(project, dir, edit, outFile) {
  const e = edit;
  const fps = [24, 30, 60].includes(+e.fps) ? +e.fps : 30;
  const { w, h } = canvasSize(e.aspect, e.quality);
  const segs = keepSegments(e.trimStart, e.trimEnd, project.timing && project.timing.pauses);
  if (!segs.length) throw new Error('Nothing to export: the selection is empty.');
  const total = segs.reduce((s, [a, b]) => s + (b - a), 0);
  const sel = segs.map(([a, b]) => `gte(t,${f(a)})*lt(t,${f(b)})`).join('+') // half-open: no extra frame per cut;
  const sync = project.sync || {};

  const hasCam = !!project.camera;
  const hasCam2 = !!project.camera2;
  const hasScreen = !!project.screen;
  let layout = e.layout || 'camera';
  if (!hasCam && !hasCam2 && !hasScreen) throw new Error('This session has no video to export.');
  if (!hasScreen) layout = 'camera';
  if (!hasCam && !hasCam2) layout = 'screen';
  const useScreen = layout !== 'camera';
  const useCams = layout !== 'screen';

  const inputs = [];
  const add = (file) => { inputs.push('-i', path.join(dir, file)); return inputs.length / 2 - 1; };
  const audioIdx = add(project.audio.file);
  const micIdx = e.includeMic && project.mic ? add(project.mic.file) : -1;
  const scrIdx = useScreen ? add(project.screen.file) : -1;
  const cams = [];
  const manual = (Number(e.manualOffsetMs) || 0) / 1000;
  if (useCams && hasCam) cams.push({ idx: add(project.camera.file), off: (sync.cameraOffsetSec || 0) + manual, rate: sync.cameraRate || 1, pre: rotateFilter(e.rotation) });
  if (useCams && hasCam2) cams.push({ idx: add(project.camera2.file), off: (sync.camera2OffsetSec || 0) + manual, rate: 1, pre: '' });

  // Put a video on the audio timeline, make it constant-rate, cut pauses/trim.
  // The pad holds the last frame if a video ends early; it must be finite or
  // ffmpeg would wait forever for frames the select below never passes.
  const padSec = segs[segs.length - 1][1] + 1;
  const onTimeline = (idx, offsetSec, rate, pre, label) =>
    `[${idx}:v]${pre}setpts=(PTS-STARTPTS)*${f(rate)}+${f(offsetSec)}/TB,trim=start=0,` +
    `fps=${fps}:start_time=0,tpad=stop_mode=clone:stop_duration=${f(padSec)},` +
    `select='${sel}',setpts=N/(${fps}*TB)[${label}]`;
  const fill = (W, H) => `scale=${W}:${H}:force_original_aspect_ratio=increase,crop=${W}:${H},setsar=1`;
  // The DAW screen is never cropped: it is scaled to fit and centred.
  const fit = (W, H) => `scale=${W}:${H}:force_original_aspect_ratio=decrease:force_divisible_by=2,` +
    `pad=${W}:${H}:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1`;

  const chains = [];
  if (scrIdx >= 0) chains.push(onTimeline(scrIdx, sync.screenOffsetSec || 0, 1, '', 'scr'));
  cams.forEach((c, i) => chains.push(onTimeline(c.idx, c.off, c.rate, c.pre, `cam${i}`)));

  // Black canvas, then the DAW screen, then each camera on top.
  const L = frameBoxes(w, h, { screen: useScreen, layout, pipSize: e.pipSize, pipCorner: e.pipCorner, splitSide: e.splitSide, splitRatio: e.splitRatio }, cams.length);
  chains.push(`color=c=black:s=${w}x${h}:r=${fps}:d=${f(total)}[base0]`);
  let base = 'base0', step = 0;
  const layer = (label, box) => {
    const next = `base${++step}`;
    chains.push(`[${base}][${label}]overlay=${box.x}:${box.y}[${next}]`);
    base = next;
  };
  if (L.scr) { chains.push(`[scr]${cropFilter(e.screenCrop)}${fit(L.scr.w, L.scr.h)}[s]`); layer('s', L.scr); }
  L.cams.forEach((box, i) => {
    if (box.outline) {
      const b = Math.max(2, even(box.w * 0.01)); // thin outline so the box reads against a dark DAW
      chains.push(`[cam${i}]${fill(box.w - 2 * b, box.h - 2 * b)},pad=${box.w}:${box.h}:${b}:${b}:color=0x2ff3e0[c${i}]`);
    } else chains.push(`[cam${i}]${fill(box.w, box.h)}[c${i}]`);
    layer(`c${i}`, box);
  });
  chains.push(`[${base}]null[v]`);

  // Audio: sample-accurate cuts, never time-stretched.
  const cutAudio = (idx, label) => {
    if (segs.length === 1) {
      return [`[${idx}:a]atrim=start=${f(segs[0][0])}:end=${f(segs[0][1])},asetpts=PTS-STARTPTS[${label}]`];
    }
    const parts = segs.map(([a, b], i) =>
      `[${idx}:a]atrim=start=${f(a)}:end=${f(b)},asetpts=PTS-STARTPTS[${label}${i}]`);
    parts.push(`${segs.map((_, i) => `[${label}${i}]`).join('')}concat=n=${segs.length}:v=0:a=1[${label}]`);
    return parts;
  };
  chains.push(...cutAudio(audioIdx, micIdx >= 0 ? 'daw' : 'a'));
  if (micIdx >= 0) {
    chains.push(...cutAudio(micIdx, 'mic'));
    chains.push(`[daw][mic]amix=inputs=2:normalize=0:duration=first[a]`);
  }

  return {
    total,
    args: ['-y', '-hide_banner', ...inputs,
      '-filter_complex', chains.join(';'),
      '-map', '[v]', '-map', '[a]',
      '-t', f(total), '-r', String(fps),
      '-c:v', 'libx264', '-preset', 'medium', '-crf', '18', '-pix_fmt', 'yuv420p',
      '-c:a', 'aac', '-b:a', '320k', '-ar', '48000',
      '-movflags', '+faststart', '-progress', 'pipe:1', '-nostats', outFile],
  };
}

/** Trimmed WAV export in the session's native format (no re-quantisation). */
function buildWavArgs(project, dir, edit, outFile) {
  const segs = keepSegments(edit.trimStart, edit.trimEnd, project.timing && project.timing.pauses);
  if (!segs.length) throw new Error('Nothing to export: the selection is empty.');
  const total = segs.reduce((s, [a, b]) => s + (b - a), 0);
  const codec = project.audio.bitDepth === 32 ? 'pcm_f32le' : 'pcm_s24le';
  const parts = segs.map(([a, b], i) => `[0:a]atrim=start=${f(a)}:end=${f(b)},asetpts=PTS-STARTPTS[s${i}]`);
  const graph = segs.length === 1
    ? parts[0].replace('[s0]', '[a]')
    : parts.join(';') + ';' + segs.map((_, i) => `[s${i}]`).join('') + `concat=n=${segs.length}:v=0:a=1[a]`;
  return {
    total,
    args: ['-y', '-hide_banner', '-i', path.join(dir, project.audio.file), '-filter_complex', graph,
      '-map', '[a]', '-c:a', codec, '-ar', String(project.audio.sampleRate),
      '-progress', 'pipe:1', '-nostats', outFile],
  };
}

function runFfmpeg(args, total, onProgress) {
  return new Promise((resolve, reject) => {
    const p = spawn(ffmpegPath(), args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => { err = (err + d).slice(-4000); });
    p.stdout.on('data', (d) => {
      const m = /out_time_(?:us|ms)=(\d+)/.exec(String(d));
      if (m && total && onProgress) onProgress(Math.min(1, Number(m[1]) / 1e6 / total));
    });
    p.on('error', (e) => reject(new Error(`Could not run ffmpeg: ${e.message}`)));
    p.on('close', (code) => (code === 0 ? resolve() : reject(new Error(`ffmpeg failed (${code}): ${err.trim().split('\n').slice(-3).join(' | ')}`))));
  });
}

/** Lossless container rewrite so recordings are seekable; streams are copied untouched. */
async function remux(src, dst, isMp4) {
  const args = ['-y', '-hide_banner', '-fflags', '+genpts', '-i', src, '-map', '0:v:0', '-c', 'copy'];
  if (isMp4) args.push('-movflags', '+faststart');
  args.push(dst);
  await runFfmpeg(args);
}

module.exports = { validCrop, frameBoxes, pipBox, splitBoxes, buildMp4Args, buildWavArgs, runFfmpeg, remux, canvasSize, ffmpegPath };
