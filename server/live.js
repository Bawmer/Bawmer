'use strict';
// Live streaming: the studio sends one composited WebM stream (DAW screen +
// camera + DAW audio); ffmpeg re-encodes it to what Instagram / TikTok / any
// RTMP(S) ingest expects (H.264 + AAC in FLV, fixed 2 s keyframes).
const { spawn } = require('child_process');
const { ffmpegPath } = require('./exporter');

function validTarget(url) {
  try {
    const u = new URL(url);
    return (u.protocol === 'rtmp:' || u.protocol === 'rtmps:') && !!u.hostname;
  } catch { return false; }
}

/** Join server URL and stream key the way OBS does. */
function targetUrl(server, key) {
  const s = String(server || '').trim();
  const k = String(key || '').trim();
  if (!k) return s;
  return s.endsWith('/') ? s + k : `${s}/${k}`;
}

function buildLiveArgs({ width, height, fps, bitrateKbps }, target) {
  const kbps = Math.max(1000, Math.min(9000, Number(bitrateKbps) || 4500));
  const r = [24, 30, 60].includes(+fps) ? +fps : 30;
  return ['-hide_banner', '-loglevel', 'warning',
    '-fflags', '+genpts+nobuffer', '-thread_queue_size', '1024', '-i', 'pipe:0',
    '-map', '0:v:0', '-map', '0:a:0?',
    '-vf', `scale=${width}:${height}:force_original_aspect_ratio=decrease,pad=${width}:${height}:(ow-iw)/2:(oh-ih)/2,fps=${r},format=yuv420p`,
    '-c:v', 'libx264', '-preset', 'veryfast', '-tune', 'zerolatency', '-profile:v', 'high',
    '-b:v', `${kbps}k`, '-maxrate', `${kbps}k`, '-bufsize', `${kbps * 2}k`,
    '-g', String(r * 2), '-keyint_min', String(r * 2), '-sc_threshold', '0',
    '-c:a', 'aac', '-b:a', '160k', '-ar', '48000', '-ac', '2',
    '-f', 'flv', '-flvflags', 'no_duration_filesize', target];
}

class LiveSession {
  constructor(opts, onEvent) {
    this.opts = opts;
    this.onEvent = onEvent;
    this.target = targetUrl(opts.server, opts.key);
    this.key = String(opts.key || '').trim();
    this.proc = null;
    this.bytes = 0;
    this.startedAt = Date.now();
    this.stopping = false;
  }

  redact(text) {
    let t = String(text);
    if (this.key) t = t.split(this.key).join('••••');
    return t;
  }

  start() {
    if (!validTarget(this.target)) throw new Error('The server URL must start with rtmp:// or rtmps://');
    const args = buildLiveArgs(this.opts, this.target);
    const p = spawn(ffmpegPath(), args, { stdio: ['pipe', 'ignore', 'pipe'] });
    this.proc = p;
    let err = '';
    p.stdin.on('error', () => {}); // EPIPE when ffmpeg quits first
    p.stderr.on('data', (d) => { err = (err + d).slice(-3000); });
    p.on('error', (e) => this.onEvent({ type: 'live.ended', ok: false, message: `Could not start ffmpeg: ${e.message}` }));
    p.on('close', (code) => {
      this.proc = null;
      const lines = this.redact(err.trim()).split('\n').filter(Boolean);
      const last = lines.slice(-2).join(' | ');
      this.onEvent({
        type: 'live.ended', ok: this.stopping, code,
        message: this.stopping ? 'Live stream ended.' : explain(last) || `The stream stopped (ffmpeg ${code}).`,
      });
    });
    this.timer = setInterval(() => {
      this.onEvent({ type: 'live.stats', seconds: Math.round((Date.now() - this.startedAt) / 1000), kbps: Math.round((this.bytes * 8) / 1000 / 2) });
      this.bytes = 0;
    }, 2000);
  }

  write(buf) {
    if (!this.proc || this.stopping) return;
    this.bytes += buf.length;
    this.proc.stdin.write(buf);
  }

  stop() {
    this.stopping = true;
    clearInterval(this.timer);
    const p = this.proc;
    if (!p) return;
    try { p.stdin.end(); } catch { /* already closed */ }
    setTimeout(() => { if (this.proc === p) p.kill('SIGKILL'); }, 4000);
  }
}

function explain(msg) {
  if (!msg) return '';
  if (/Connection refused|Network is unreachable|No route|timed out|Failed to resolve|Name or service not known/i.test(msg)) {
    return 'Could not reach the streaming server. Check the internet connection and the server URL.';
  }
  if (/Input\/output error|Broken pipe|end of file/i.test(msg)) {
    return 'The platform closed the connection. The stream key may be wrong or expired — Instagram and TikTok keys are usually valid for one broadcast only.';
  }
  if (/TLS|handshake|certificate/i.test(msg)) return 'Secure connection to the streaming server failed (check the rtmps:// URL).';
  return msg;
}

module.exports = { LiveSession, buildLiveArgs, targetUrl, validTarget };
