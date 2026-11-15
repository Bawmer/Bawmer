'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { WavWriter, HEADER_BYTES } = require('../../server/wav');
const { computeSync, keepSegments } = require('../../server/sync');
const { buildMp4Args, canvasSize, pipBox } = require('../../server/exporter');

test('wav writer produces a valid 24-bit header and samples', () => {
  const file = path.join(os.tmpdir(), `dlc-${process.pid}.wav`);
  const w = new WavWriter(file, { sampleRate: 48000, channels: 2, bitDepth: 24 });
  w.write(new Float32Array([0.5, -0.5, 1, -1]));
  w.close();
  const b = fs.readFileSync(file);
  fs.rmSync(file);
  assert.equal(b.toString('ascii', 0, 4), 'RIFF');
  assert.equal(b.readUInt32LE(4), b.length - 8);
  assert.equal(b.readUInt16LE(70), 24);
  assert.equal(b.readUInt32LE(76), 12);
  assert.equal(b.readIntLE(HEADER_BYTES, 3), 4194304);
  assert.equal(b.readIntLE(HEADER_BYTES + 3, 3), -4194303);
  assert.equal(b.readIntLE(HEADER_BYTES + 9, 3), -8388607);
  assert.equal(w.frames, 2);
});

test('sync: start offsets and calibration', () => {
  const s = computeSync({ audioStartMs: 1000, cameraStartMs: 1250, screenStartMs: 900,
    calibration: { videoLatencyMs: 80, audioLatencyMs: 20 } }, 48000);
  assert.ok(Math.abs(s.cameraOffsetSec - 0.19) < 1e-9);
  assert.ok(Math.abs(s.screenOffsetSec - -0.08) < 1e-9);
  assert.equal(s.cameraRate, 1);
});

test('sync: drift between phone clock and audio clock is recovered', () => {
  const sr = 48000, audioMarkers = [], cameraMarkers = [];
  // Audio interface runs 50 ppm fast, phone clock 30 ppm slow, over 10 minutes.
  for (let t = 0; t <= 600000; t += 5000) {
    audioMarkers.push([Math.round((t / 1000) * sr * 1.00005), 5000 + t + (Math.random() - 0.5) * 4]);
    cameraMarkers.push([t * 0.99997, 5300 + t + (Math.random() - 0.5) * 4]);
  }
  const s = computeSync({ audioStartMs: 5000, cameraStartMs: 5300, audioMarkers, cameraMarkers }, sr);
  assert.ok(s.driftCorrected);
  assert.ok(Math.abs(s.cameraRate - (1 / 0.99997) / (1 / 1.00005)) < 5e-6, String(s.cameraRate));
  assert.ok(Math.abs(s.cameraOffsetSec - 0.3) < 0.005);
});

test('pauses are cut out of the kept segments', () => {
  assert.deepEqual(keepSegments(1, 10, [[3, 4], [0, 1.5], [9, 12]]), [[1.5, 3], [4, 9]]);
});

test('mp4 export graph', () => {
  assert.deepEqual(canvasSize('9:16', 1080), { w: 1080, h: 1920 });
  const project = { audio: { file: 'audio.wav', sampleRate: 48000, bitDepth: 24 }, camera: { file: 'camera.webm' },
    screen: { file: 'screen.webm' }, timing: { pauses: [[2, 3]] }, sync: { cameraOffsetSec: 0.25, cameraRate: 1.00002, screenOffsetSec: 0 } };
  const { args, total } = buildMp4Args(project, '/s', { trimStart: 0, trimEnd: 5, aspect: '16:9', quality: 1080, fps: 30, layout: 'pip', manualOffsetMs: -50 }, '/s/out.mp4');
  const graph = args[args.indexOf('-filter_complex') + 1];
  assert.equal(total, 4);
  assert.match(graph, /setpts=\(PTS-STARTPTS\)\*1\.000020\+0\.200000\/TB/);
  // Full DAW screen is fitted (never cropped); camera is a small square, bottom-right by default.
  assert.match(graph, /\[scr\]scale=1920:1080:force_original_aspect_ratio=decrease[^;]*pad=1920:1080/);
  assert.match(graph, /\[cam\]scale=294:294:force_original_aspect_ratio=increase,crop=294:294,setsar=1,pad=302:302:4:4/);
  assert.match(graph, /overlay=1586:746\[v\]/);
  assert.match(graph, /concat=n=2:v=0:a=1/);
});

test('square camera box: sizes and corners', () => {
  assert.deepEqual(pipBox(1920, 1080, 'small', 'br'), { size: 302, x: 1586, y: 746 });
  assert.deepEqual(pipBox(1920, 1080, 'large', 'tl'), { size: 496, x: 32, y: 32 });
  assert.deepEqual(pipBox(1080, 1920, 'medium', 'tr'), { size: 388, x: 660, y: 32 });
  assert.deepEqual(pipBox(1280, 720, undefined, 'nope'), pipBox(1280, 720, 'small', 'br'));
});
