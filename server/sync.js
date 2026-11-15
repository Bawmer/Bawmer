'use strict';
// Audio/video synchronisation maths.
//
// Everything is expressed on the desktop master clock (ms). Audio is the
// master timeline: it is never resampled. The camera is mapped onto it with an
// offset plus a rate factor that absorbs clock drift between the phone and the
// audio interface over long takes.

const MIN_DRIFT_SPAN_MS = 60000; // below this, drift estimates are noise
const MAX_RATE_DEVIATION = 0.001; // crystal clocks are far tighter than 1000 ppm

/** Least-squares fit y = a + b·x. */
function fitLine(points) {
  const n = points.length;
  let sx = 0, sy = 0;
  for (const [x, y] of points) { sx += x; sy += y; }
  const mx = sx / n, my = sy / n;
  let sxx = 0, sxy = 0;
  for (const [x, y] of points) { sxx += (x - mx) * (x - mx); sxy += (x - mx) * (y - my); }
  const b = sxx ? sxy / sxx : 1;
  return { a: my - b * mx, b };
}

/** Clock model t = start + rate·media from markers [[mediaMs, clockMs], …]. */
function clockModel(markers, startMs) {
  const pts = (markers || []).filter((m) => Array.isArray(m) && m.every(Number.isFinite));
  if (pts.length >= 4 && pts[pts.length - 1][0] - pts[0][0] >= MIN_DRIFT_SPAN_MS) {
    const { a, b } = fitLine(pts);
    if (Math.abs(b - 1) <= MAX_RATE_DEVIATION) return { start: a, rate: b, drift: true };
  }
  return { start: startMs, rate: 1, drift: false };
}

/**
 * @param timing {audioStartMs, audioMarkers:[[samples, ms]], cameraStartMs,
 *                cameraMarkers:[[mediaMs, ms]], screenStartMs, calibration}
 * @returns offsets (seconds on the audio timeline at which each video's
 *          media-time zero falls) and the camera rate factor.
 */
function computeSync(timing, sampleRate) {
  const t = timing || {};
  const cal = t.calibration || {};
  const videoLatency = Number(cal.videoLatencyMs) || 0;
  const audioLatency = Number(cal.audioLatencyMs) || 0;
  const audio = clockModel(
    (t.audioMarkers || []).map(([samples, ms]) => [(samples / sampleRate) * 1000, ms]),
    t.audioStartMs
  );
  const out = { cameraOffsetSec: 0, cameraRate: 1, screenOffsetSec: 0, driftCorrected: false };
  if (!Number.isFinite(audio.start)) return out;
  if (Number.isFinite(t.cameraStartMs)) {
    const cam = clockModel(t.cameraMarkers, t.cameraStartMs);
    out.cameraRate = cam.rate / audio.rate;
    out.cameraOffsetSec = ((cam.start - audio.start) / audio.rate - videoLatency + audioLatency) / 1000;
    out.driftCorrected = cam.drift || audio.drift;
  }
  if (Number.isFinite(t.camera2StartMs)) {
    // USB camera recorded on this computer: same clock as the audio.
    out.camera2OffsetSec = ((t.camera2StartMs - audio.start) / audio.rate - (Number(cal.camera2LatencyMs) || 0) + audioLatency) / 1000;
  }
  if (Number.isFinite(t.screenStartMs)) {
    out.screenOffsetSec = ((t.screenStartMs - audio.start) / audio.rate + audioLatency) / 1000;
  }
  return out;
}

/** Segments of [start,end] that survive after removing the paused ranges. */
function keepSegments(start, end, pauses) {
  let segs = [[start, end]];
  for (const [p0, p1] of pauses || []) {
    const next = [];
    for (const [a, b] of segs) {
      if (p1 <= a || p0 >= b) { next.push([a, b]); continue; }
      if (p0 > a) next.push([a, p0]);
      if (p1 < b) next.push([p1, b]);
    }
    segs = next;
  }
  return segs.filter(([a, b]) => b - a > 0.02);
}

module.exports = { computeSync, keepSegments, fitLine, clockModel };
