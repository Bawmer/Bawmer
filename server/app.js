'use strict';
const http = require('http');
const https = require('https');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawn } = require('child_process');
const { WebSocketServer } = require('ws');
const QRCode = require('qrcode');
const { Store } = require('./sessions');
const { WavWriter } = require('./wav');
const { computeSync } = require('./sync');
const { buildMp4Args, buildWavArgs, runFfmpeg, remux } = require('./exporter');
const { lanAddresses, loadCertificate } = require('./net');
const { detectDaws } = require('./daw');

const PUBLIC = path.join(__dirname, '..', 'public');
const KIND = { DAW: 1, MIC: 2, SCREEN: 3, CAMERA: 4 };
const TYPES = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.webmanifest': 'application/manifest+json',
  '.wav': 'audio/wav', '.mp4': 'video/mp4', '.webm': 'video/webm', '.mkv': 'video/x-matroska',
};
const MODE_DEFAULTS = {
  studio: { aspect: '16:9', layout: 'pip' },
  performance: { aspect: '16:9', layout: 'camera' },
  tutorial: { aspect: '16:9', layout: 'split' },
  social: { aspect: '9:16', layout: 'camera' },
};

function sendJson(res, code, body) {
  res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
  res.end(JSON.stringify(body));
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let data = '';
    req.on('data', (c) => { data += c; if (data.length > 1 << 20) { reject(new Error('Body too large')); req.destroy(); } });
    req.on('end', () => { try { resolve(data ? JSON.parse(data) : {}); } catch (e) { reject(e); } });
    req.on('error', reject);
  });
}

/** Serve a file with HTTP Range support (needed to seek long WAV/MP4 files). */
function sendFile(req, res, file, extraHeaders = {}) {
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) { res.writeHead(404); res.end('Not found'); return; }
    const headers = {
      'Content-Type': TYPES[path.extname(file).toLowerCase()] || 'application/octet-stream',
      'Accept-Ranges': 'bytes', 'Cache-Control': 'no-cache', ...extraHeaders,
    };
    const m = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || '');
    if (m && (m[1] || m[2])) {
      let start = m[1] ? Number(m[1]) : st.size - Number(m[2]);
      let end = m[1] && m[2] ? Number(m[2]) : st.size - 1;
      start = Math.max(0, start); end = Math.min(end, st.size - 1);
      if (start > end) { res.writeHead(416, { 'Content-Range': `bytes */${st.size}` }); res.end(); return; }
      res.writeHead(206, { ...headers, 'Content-Range': `bytes ${start}-${end}/${st.size}`, 'Content-Length': end - start + 1 });
      if (req.method === 'HEAD') return res.end();
      fs.createReadStream(file, { start, end }).pipe(res);
    } else {
      res.writeHead(200, { ...headers, 'Content-Length': st.size });
      if (req.method === 'HEAD') return res.end();
      fs.createReadStream(file).pipe(res);
    }
  });
}

function safeJoin(root, rel) {
  const file = path.normalize(path.join(root, rel));
  return file === root || file.startsWith(root + path.sep) ? file : null;
}

function serveStatic(req, res, root, rel) {
  let file = safeJoin(root, decodeURIComponent(rel));
  if (!file) { res.writeHead(403); res.end(); return; }
  if (rel === '' || rel.endsWith('/')) file = path.join(file, 'index.html');
  sendFile(req, res, file);
}

async function start(opts = {}) {
  const dataDir = opts.dataDir || process.env.DLC_DATA_DIR || path.join(os.homedir(), 'DAW Live Cam');
  fs.mkdirSync(dataDir, { recursive: true });
  const store = new Store(dataDir);
  if (!store.settings().pairToken) store.saveSettings({ pairToken: crypto.randomBytes(9).toString('base64url') });

  let desktop = null; // WebSocket of the desktop UI
  let phone = null; // WebSocket of the paired phone
  let rec = null; // active recording
  const ports = { local: 0, lan: 0 };

  const toDesktop = (msg) => { if (desktop && desktop.readyState === 1) desktop.send(JSON.stringify(msg)); };
  const toPhone = (msg) => { if (phone && phone.readyState === 1) phone.send(JSON.stringify(msg)); };

  const pairUrls = () => {
    const token = store.settings().pairToken;
    return lanAddresses().map((a) => ({ ...a, url: `https://${a.address}:${ports.lan}/m/?t=${token}` }));
  };

  // ---------------------------------------------------------------- recording
  function startRecording(msg) {
    if (rec) throw new Error('A recording is already running.');
    const sampleRate = [44100, 48000, 96000].includes(+msg.audio?.sampleRate) ? +msg.audio.sampleRate : 48000;
    const bitDepth = +msg.audio?.bitDepth === 32 ? 32 : 24;
    const mode = MODE_DEFAULTS[msg.mode] ? msg.mode : 'studio';
    const project = store.create({
      title: String(msg.title || 'Untitled session').slice(0, 120), mode,
      audio: { file: 'audio.wav', sampleRate, bitDepth, channels: 2 },
    });
    const dir = store.dir(project.id);
    rec = {
      project, dir, cam: null, screen: null, camDone: null,
      daw: new WavWriter(path.join(dir, 'audio.wav'), { sampleRate, bitDepth, channels: 2 }),
      mic: msg.mic ? new WavWriter(path.join(dir, 'mic.wav'), { sampleRate, bitDepth, channels: 2 }) : null,
    };
    return project.id;
  }

  function openVideo(name, mime) {
    const isMp4 = /mp4|avc|h264/i.test(mime || '');
    const raw = `${name}.raw.${/mp4/i.test(mime || '') ? 'mp4' : 'webm'}`;
    return { raw, isMp4, mime, fd: fs.openSync(path.join(rec.dir, raw), 'w'), bytes: 0, seq: 0, done: false };
  }

  async function finishVideo(v, name) {
    if (!v) return null;
    if (v.fd != null) { fs.closeSync(v.fd); v.fd = null; }
    const rawPath = path.join(rec.dir, v.raw);
    if (!v.bytes) { fs.rmSync(rawPath, { force: true }); return null; }
    const out = `${name}.${v.isMp4 ? 'mp4' : 'webm'}`;
    try {
      await remux(rawPath, path.join(rec.dir, out), v.isMp4);
      fs.rmSync(rawPath, { force: true });
      return out;
    } catch (e) {
      console.warn(`[dlc] could not finalise ${name} container, keeping the raw stream:`, e.message);
      return v.raw;
    }
  }

  async function stopRecording(msg) {
    const r = rec;
    if (!r || r.stopping) throw new Error('No recording is running.');
    r.stopping = true;
    try {
      r.daw.close();
      if (r.mic) r.mic.close();
      // Give the phone a moment to flush its last chunks (it may be reconnecting).
      if (r.cam && !r.cam.done) {
        await new Promise((resolve) => { r.camDone = resolve; setTimeout(resolve, 20000); });
      }
      const p = r.project;
      const camFile = await finishVideo(r.cam, 'camera');
      const scrFile = await finishVideo(r.screen, 'screen');
      p.audio.frames = r.daw.frames;
      p.duration = r.daw.frames / p.audio.sampleRate;
      if (r.mic && r.mic.frames) p.mic = { file: 'mic.wav' }; else if (r.mic) fs.rmSync(path.join(r.dir, 'mic.wav'), { force: true });
      const cm = msg.camera || {};
      if (camFile) p.camera = { file: camFile, mime: r.cam.mime, width: cm.width || null, height: cm.height || null, fps: cm.fps || null };
      if (scrFile) p.screen = { file: scrFile };
      p.timing = msg.timing || {};
      p.sync = computeSync(p.timing, p.audio.sampleRate);
      p.edit = {
        trimStart: 0, trimEnd: p.duration, manualOffsetMs: 0, rotation: Number(cm.rotation) || 0,
        quality: 1080, fps: [24, 30, 60].includes(cm.fps) ? cm.fps : 30, includeMic: !!p.mic,
        ...MODE_DEFAULTS[p.mode], ...(msg.preset || {}),
      };
      p.status = 'ready';
      fs.writeFileSync(path.join(r.dir, 'peaks.json'), JSON.stringify(r.daw.peaks));
      store.save(p);
      return p.id;
    } finally {
      rec = null;
    }
  }

  function onBinary(role, buf) {
    if (!rec || buf.length < 8) return;
    const kind = buf.readUInt32LE(0);
    if (role === 'desktop') {
      if (kind === KIND.DAW || kind === KIND.MIC) {
        const w = kind === KIND.DAW ? rec.daw : rec.mic;
        if (!w || rec.stopping) return;
        const bytes = (buf.length - 4) & ~3;
        const copy = new Uint8Array(bytes); // realign for Float32Array
        copy.set(buf.subarray(4, 4 + bytes));
        w.write(new Float32Array(copy.buffer));
      } else if (kind === KIND.SCREEN) {
        if (!rec.screen) rec.screen = openVideo('screen', 'video/webm');
        if (rec.screen.fd == null) return;
        fs.writeSync(rec.screen.fd, buf, 4, buf.length - 4);
        rec.screen.bytes += buf.length - 4;
      }
    } else if (kind === KIND.CAMERA && rec.cam && rec.cam.fd != null) {
      const seq = buf.readUInt32LE(4);
      if (seq === rec.cam.seq + 1) {
        fs.writeSync(rec.cam.fd, buf, 8, buf.length - 8);
        rec.cam.bytes += buf.length - 8;
        rec.cam.seq = seq;
      }
      toPhone({ type: 'cam.ack', seq: rec.cam.seq }); // duplicates and gaps re-ack the last good chunk
    }
  }

  async function onDesktopMessage(msg) {
    try {
      if (msg.type === 'rec.start') toDesktop({ type: 'rec.started', id: startRecording(msg) });
      else if (msg.type === 'rec.stop') toDesktop({ type: 'rec.saved', id: await stopRecording(msg) });
      else toPhone(msg);
    } catch (e) {
      toDesktop({ type: 'rec.error', message: e.message });
    }
  }

  function onPhoneMessage(msg) {
    if (msg.type === 'cam.begin') {
      if (rec && !rec.cam && !rec.stopping && msg.sessionId === rec.project.id) rec.cam = openVideo('camera', msg.mime);
    } else if (msg.type === 'cam.done') {
      if (rec && rec.cam && msg.sessionId === rec.project.id && msg.chunks === rec.cam.seq) {
        rec.cam.done = true;
        if (rec.camDone) rec.camDone();
      }
    }
    toDesktop(msg);
  }

  // ------------------------------------------------------------------ exports
  const exportsRunning = new Set();
  async function runExport(id, body) {
    const project = store.load(id);
    const dir = store.dir(id);
    const edit = { ...project.edit, ...body.edit };
    const kind = body.kind === 'wav' ? 'wav' : 'mp4';
    const base = (project.title || 'session').replace(/[^\w\- ]+/g, '').trim().replace(/\s+/g, '-') || 'session';
    const tag = kind === 'wav' ? 'audio' : `${edit.aspect.replace(':', 'x')}-${edit.quality}p`;
    let n = 1, name;
    do { name = `${base}-${tag}${n > 1 ? `-${n}` : ''}.${kind}`; n++; } while (fs.existsSync(path.join(dir, 'exports', name)));
    const out = path.join(dir, 'exports', name);
    const { args, total } = kind === 'wav' ? buildWavArgs(project, dir, edit, out) : buildMp4Args(project, dir, edit, out);
    exportsRunning.add(id);
    runFfmpeg(args, total, (p) => toDesktop({ type: 'export.progress', id, name, progress: p }))
      .then(() => {
        const fresh = store.load(id);
        fresh.exports = [...(fresh.exports || []), { file: `exports/${name}`, kind, createdAt: new Date().toISOString(), size: fs.statSync(out).size }];
        store.save(fresh);
        toDesktop({ type: 'export.done', id, name, file: `exports/${name}`, path: out });
      })
      .catch((e) => { fs.rmSync(out, { force: true }); toDesktop({ type: 'export.error', id, name, message: e.message }); })
      .finally(() => exportsRunning.delete(id));
    return name;
  }

  // --------------------------------------------------------------- local HTTP
  const localHost = (req) => /^(127\.0\.0\.1|localhost)(:\d+)?$/.test(req.headers.host || '');

  async function localHandler(req, res) {
    // Only the machine itself may use the desktop API (also blocks DNS rebinding).
    if (!localHost(req)) { res.writeHead(403); res.end('Forbidden'); return; }
    const url = new URL(req.url, 'http://localhost');
    const p = url.pathname;
    try {
      if (p === '/api/state') {
        const s = store.settings();
        return sendJson(res, 200, {
          pairUrls: pairUrls(), phoneConnected: !!phone, storage: await store.storage(), dataDir,
          platform: process.platform, electron: !!opts.electron, recording: rec ? rec.project.id : null,
          calibration: s.calibration || null, prefs: s.prefs || {},
        });
      }
      if (p === '/api/daws') return sendJson(res, 200, await detectDaws());
      if (p === '/api/qr.svg') {
        const svg = await QRCode.toString(url.searchParams.get('u') || '', { type: 'svg', margin: 1, color: { dark: '#0b0d12', light: '#ffffff' } });
        res.writeHead(200, { 'Content-Type': 'image/svg+xml', 'Cache-Control': 'no-store' });
        return res.end(svg);
      }
      if (p === '/api/settings' && req.method === 'PUT') {
        const body = await readBody(req);
        const patch = {};
        if ('calibration' in body) patch.calibration = body.calibration;
        if ('prefs' in body) patch.prefs = body.prefs;
        store.saveSettings(patch);
        return sendJson(res, 200, { ok: true });
      }
      if (p === '/api/pair/reset' && req.method === 'POST') {
        store.saveSettings({ pairToken: crypto.randomBytes(9).toString('base64url') });
        if (phone) phone.close(4001, 'unpaired');
        return sendJson(res, 200, { pairUrls: pairUrls() });
      }
      if (p === '/api/sessions' && req.method === 'GET') return sendJson(res, 200, store.list());
      let m = /^\/api\/sessions\/([\w-]+)$/.exec(p);
      if (m) {
        if (req.method === 'GET') {
          const project = store.load(m[1]);
          let peaks = [];
          try { peaks = JSON.parse(fs.readFileSync(path.join(store.dir(m[1]), 'peaks.json'), 'utf8')); } catch { /* optional */ }
          return sendJson(res, 200, { project, peaks, dir: store.dir(m[1]) });
        }
        if (req.method === 'PATCH') {
          const body = await readBody(req);
          const project = store.load(m[1]);
          if (typeof body.title === 'string') project.title = body.title.slice(0, 120);
          if (body.edit && typeof body.edit === 'object') project.edit = { ...project.edit, ...body.edit };
          store.save(project);
          return sendJson(res, 200, { ok: true });
        }
        if (req.method === 'DELETE') {
          if (rec && rec.project.id === m[1]) return sendJson(res, 409, { error: 'Session is recording.' });
          if (exportsRunning.has(m[1])) return sendJson(res, 409, { error: 'An export is still running.' });
          store.remove(m[1]);
          return sendJson(res, 200, { ok: true });
        }
      }
      m = /^\/api\/sessions\/([\w-]+)\/export$/.exec(p);
      if (m && req.method === 'POST') return sendJson(res, 200, { name: await runExport(m[1], await readBody(req)) });
      m = /^\/api\/sessions\/([\w-]+)\/reveal$/.exec(p);
      if (m && req.method === 'POST') {
        const target = path.join(store.dir(m[1]), 'exports');
        if (opts.reveal) opts.reveal(target);
        else spawn(process.platform === 'win32' ? 'explorer' : process.platform === 'darwin' ? 'open' : 'xdg-open', [target], { stdio: 'ignore', detached: true }).on('error', () => {}).unref();
        return sendJson(res, 200, { path: target });
      }
      m = /^\/media\/([\w-]+)\/(.+)$/.exec(p);
      if (m) {
        const file = safeJoin(store.dir(m[1]), decodeURIComponent(m[2]));
        if (!file) { res.writeHead(403); return res.end(); }
        const dl = url.searchParams.has('dl') ? { 'Content-Disposition': `attachment; filename="${path.basename(file)}"` } : {};
        return sendFile(req, res, file, dl);
      }
      if (p.startsWith('/m/')) return serveStatic(req, res, path.join(PUBLIC, 'mobile'), p.slice(3));
      if (p.startsWith('/api/')) return sendJson(res, 404, { error: 'Not found' });
      return serveStatic(req, res, path.join(PUBLIC, 'desktop'), p.slice(1));
    } catch (e) {
      const missing = e.code === 'ENOENT';
      return sendJson(res, missing ? 404 : 400, { error: missing ? 'Not found' : e.message });
    }
  }

  // The LAN listener exposes nothing but the phone UI and the paired phone socket.
  function lanHandler(req, res) {
    const p = new URL(req.url, 'https://localhost').pathname;
    if (p.startsWith('/m/')) return serveStatic(req, res, path.join(PUBLIC, 'mobile'), p.slice(3));
    res.writeHead(302, { Location: '/m/' });
    res.end();
  }

  // --------------------------------------------------------------- websockets
  const wss = new WebSocketServer({ noServer: true, maxPayload: 256 << 20 });

  function attach(ws, role) {
    const isDesktop = role === 'desktop';
    const prev = isDesktop ? desktop : phone;
    if (prev && prev !== ws) prev.close(4000, 'replaced');
    if (isDesktop) desktop = ws; else phone = ws;
    ws.isAlive = true;
    ws.on('pong', () => { ws.isAlive = true; });
    ws.on('message', (data, isBinary) => {
      if (isBinary) return onBinary(role, data);
      let msg;
      try { msg = JSON.parse(data.toString()); } catch { return; }
      if (!msg || typeof msg.type !== 'string') return;
      if (isDesktop) onDesktopMessage(msg); else onPhoneMessage(msg);
    });
    ws.on('close', () => {
      if (isDesktop && desktop === ws) { desktop = null; toPhone({ type: 'peer', connected: false }); }
      if (!isDesktop && phone === ws) { phone = null; toDesktop({ type: 'peer', connected: false }); }
    });
    ws.on('error', () => {});
    // Tell both sides who is present.
    if (isDesktop) { ws.send(JSON.stringify({ type: 'peer', connected: !!phone })); toPhone({ type: 'peer', connected: true }); }
    else {
      ws.send(JSON.stringify({ type: 'peer', connected: !!desktop, camSeq: rec && rec.cam ? rec.cam.seq : 0, recording: rec ? rec.project.id : null }));
      toDesktop({ type: 'peer', connected: true });
    }
  }

  const heartbeat = setInterval(() => {
    for (const ws of [desktop, phone]) {
      if (!ws) continue;
      if (!ws.isAlive) { ws.terminate(); continue; }
      ws.isAlive = false;
      try { ws.ping(); } catch { /* closing */ }
    }
  }, 5000);

  const local = http.createServer(localHandler);
  local.on('upgrade', (req, socket, head) => {
    const origin = req.headers.origin || '';
    const okOrigin = !origin || /^https?:\/\/(127\.0\.0\.1|localhost)(:\d+)?$/.test(origin);
    if (!localHost(req) || !okOrigin || !req.url.startsWith('/ws')) return socket.destroy();
    wss.handleUpgrade(req, socket, head, (ws) => attach(ws, 'desktop'));
  });

  const lan = https.createServer(loadCertificate(dataDir), lanHandler);
  lan.on('upgrade', (req, socket, head) => {
    const url = new URL(req.url, 'https://localhost');
    const token = url.searchParams.get('t') || '';
    const expected = store.settings().pairToken;
    const ok = url.pathname === '/ws' && token.length === expected.length &&
      crypto.timingSafeEqual(Buffer.from(token), Buffer.from(expected));
    if (!ok) { socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n'); return socket.destroy(); }
    wss.handleUpgrade(req, socket, head, (ws) => attach(ws, 'phone'));
  });

  const listen = (server, host, first) => new Promise((resolve, reject) => {
    let port = first;
    const attempt = () => {
      server.once('error', (e) => {
        if (e.code === 'EADDRINUSE' && port < first + 20) { port += 2; attempt(); } else reject(e);
      });
      server.listen(port, host, () => { server.removeAllListeners('error'); resolve(port); });
    };
    attempt();
  });

  const base = Number(opts.port || process.env.DLC_PORT) || 47800;
  ports.local = await listen(local, '127.0.0.1', base);
  ports.lan = await listen(lan, '0.0.0.0', base + 1);

  return {
    ports, dataDir, localUrl: `http://127.0.0.1:${ports.local}/`, pairUrls,
    close: () => new Promise((resolve) => {
      clearInterval(heartbeat);
      for (const ws of [desktop, phone]) if (ws) ws.terminate();
      lan.close(); local.close(() => resolve());
      lan.closeAllConnections?.(); local.closeAllConnections?.();
    }),
  };
}

module.exports = { start, KIND };
