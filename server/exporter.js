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
const f = (n) => Number(n).toFixed(6);

function canvasSize(aspect, quality) {
  const q = [720, 1080, 1440, 2160].includes(+quality) ? +quality : 1080;
  if (aspect === '9:16') return { w: q, h: even((q * 16) / 9) };
  if (aspect === '1:1') return { w: q, h: q };
  return { w: even((q * 16) / 9), h: q };
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
  const hasScreen = !!project.screen;
  let layout = e.layout || 'camera';
  if (!hasCam && !hasScreen) throw new Error('This session has no video to export.');
  if (!hasScreen) layout = 'camera';
  if (!hasCam) layout = 'screen';

  const inputs = [];
  const add = (file) => { inputs.push('-i', path.join(dir, file)); return inputs.length / 2 - 1; };
  const audioIdx = add(project.audio.file);
  const micIdx = e.includeMic && project.mic ? add(project.mic.file) : -1;
  const camIdx = layout !== 'screen' ? add(project.camera.file) : -1;
  const scrIdx = layout !== 'camera' ? add(project.screen.file) : -1;

  // Put a video on the audio timeline, make it constant-rate, cut pauses/trim.
  // The pad holds the last frame if a video ends early; it must be finite or
  // ffmpeg would wait forever for frames the select below never passes.
  const padSec = segs[segs.length - 1][1] + 1;
  const onTimeline = (idx, offsetSec, rate, pre, label) =>
    `[${idx}:v]${pre}setpts=(PTS-STARTPTS)*${f(rate)}+${f(offsetSec)}/TB,trim=start=0,` +
    `fps=${fps}:start_time=0,tpad=stop_mode=clone:stop_duration=${f(padSec)},` +
    `select='${sel}',setpts=N/(${fps}*TB)[${label}]`;
  const fill = (W, H) => `scale=${W}:${H}:force_original_aspect_ratio=increase,crop=${W}:${H},setsar=1`;

  const chains = [];
  if (camIdx >= 0) {
    const off = (sync.cameraOffsetSec || 0) + (Number(e.manualOffsetMs) || 0) / 1000;
    chains.push(onTimeline(camIdx, off, sync.cameraRate || 1, rotateFilter(e.rotation), 'cam'));
  }
  if (scrIdx >= 0) chains.push(onTimeline(scrIdx, sync.screenOffsetSec || 0, 1, '', 'scr'));

  if (layout === 'camera') chains.push(`[cam]${fill(w, h)}[v]`);
  else if (layout === 'screen') chains.push(`[scr]${fill(w, h)}[v]`);
  else if (layout === 'pip') {
    const pw = even(w * (w >= h ? 0.28 : 0.42));
    const camAspect = project.camera.width && project.camera.height
      ? (e.rotation % 180 ? project.camera.width / project.camera.height : project.camera.height / project.camera.width)
      : 9 / 16;
    const ph = even(pw * camAspect);
    const m = even(w * 0.025);
    chains.push(`[scr]${fill(w, h)}[bg]`, `[cam]${fill(pw, ph)}[pip]`,
      `[bg][pip]overlay=W-w-${m}:H-h-${m}[v]`);
  } else { // split: side by side on wide canvases, stacked otherwise
    if (w > h) {
      const cw = even(w * 0.34);
      chains.push(`[scr]${fill(w - cw, h)}[a]`, `[cam]${fill(cw, h)}[b]`, `[a][b]hstack=inputs=2[v]`);
    } else {
      const sh = even(h * 0.42);
      chains.push(`[scr]${fill(w, sh)}[a]`, `[cam]${fill(w, h - sh)}[b]`, `[a][b]vstack=inputs=2[v]`);
    }
  }

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

module.exports = { buildMp4Args, buildWavArgs, runFfmpeg, remux, canvasSize, ffmpegPath };
