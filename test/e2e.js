'use strict';
// End-to-end check with real browsers and simulated capture devices:
// pair → preview → record (with a pause) → save → export MP4 + WAV.
// Needs Playwright with Chromium available (`npm i -D playwright`).
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');
const { chromium } = require('playwright');
const { start } = require('../server/app');
const { ffmpegPath } = require('../server/exporter');

const shots = process.env.DLC_SHOTS;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const assert = (cond, msg) => { if (!cond) throw new Error(`ASSERT: ${msg}`); console.log(`  ✓ ${msg}`); };

(async () => {
  const dataDir = fs.mkdtempSync(path.join(os.tmpdir(), 'dlc-e2e-'));
  const server = await start({ dataDir, port: 48800 + Math.floor(Math.random() * 500) * 2 });
  const browser = await chromium.launch({ args: [
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--ignore-certificate-errors',
    '--autoplay-policy=no-user-gesture-required', '--auto-select-desktop-capture-source=Entire screen',
  ] });
  let failed = false;
  try {
    const dctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, deviceScaleFactor: shots ? 2 : 1 });
    const pctx = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: shots ? 2 : 1, ignoreHTTPSErrors: true, isMobile: true, hasTouch: true });
    const desk = await dctx.newPage();
    const phone = await pctx.newPage();
    for (const [n, p] of [['desktop', desk], ['phone', phone]]) {
      p.on('pageerror', (e) => { console.log(`[${n}] pageerror`, e.message); failed = true; });
      p.on('console', (m) => { if (m.type() === 'error') console.log(`[${n}] console.error`, m.text()); });
    }
    await desk.goto(server.localUrl);
    await desk.waitForSelector('#srcSel option', { state: 'attached' });
    if (shots) { await desk.click('#pairBtn'); await desk.waitForSelector('#qr svg'); await desk.screenshot({ path: path.join(shots, 'pair.png') }); await desk.click('#pairDlg [data-close]'); }

    const pairUrl = `https://127.0.0.1:${server.ports.lan}/m/?t=${JSON.parse(fs.readFileSync(path.join(dataDir, 'settings.json'))).pairToken}`;
    const bad = await pctx.request.get(`https://127.0.0.1:${server.ports.lan}/api/state`, { maxRedirects: 0 });
    assert(bad.status() === 302, 'LAN listener does not expose the desktop API');
    await phone.addInitScript(() => {
      const Native = window.WebSocket;
      window.__sockets = [];
      window.WebSocket = class extends Native { constructor(...a) { super(...a); window.__sockets.push(this); } };
    });
    await phone.goto(pairUrl);
    if (await phone.isVisible('#startBtn')) await phone.click('#startBtn').catch(() => {});
    await desk.waitForFunction(() => document.getElementById('preview').videoWidth > 0, null, { timeout: 20000 });
    assert(true, 'phone paired and live WebRTC preview is showing on the desktop');
    await desk.waitForFunction(() => document.getElementById('statFormat').textContent.includes('p'));

    // audio: use the first real input device (fake device = beeps)
    await desk.evaluate(() => {
      const sel = document.getElementById('srcSel');
      sel.value = [...sel.options].find((o) => o.value !== 'system').value;
    });
    await desk.click('#audioBtn');
    await desk.waitForFunction(() => document.getElementById('audioState').textContent === 'Connected', null, { timeout: 15000 });
    const screenOn = await desk.evaluate(() => !document.getElementById('chipScreen').hidden);
    console.log(`  · screen capture active: ${screenOn}`);
    await sleep(1500);
    assert(await desk.evaluate(() => document.getElementById('peakRead').textContent !== '−∞'), 'DAW input meter shows signal');

    if (screenOn) {
      const st = await desk.evaluate(() => {
        const v = document.getElementById('preview').getBoundingClientRect();
        return { comp: document.getElementById('stage').classList.contains('composite'), w: v.width, h: v.height };
      });
      assert(st.comp, 'studio preview shows the DAW screen and the camera together');
      const two = await desk.evaluate(() => {
        const a = document.getElementById('screenMain').getBoundingClientRect(), b = document.getElementById('preview').getBoundingClientRect();
        return { split: document.getElementById('stage').classList.contains('split'), overlap: a.right > b.left + 1 && b.right > a.left + 1, ratio: a.width / b.width };
      });
      assert(two.split && !two.overlap && Math.abs(two.ratio - 2) < 0.05, 'default is two screens side by side: DAW (2/3) + camera (1/3), no overlap');
      await desk.click('#layoutStudio [data-v="pip"]');
      const sq = await desk.evaluate(() => { const r = document.getElementById('preview').getBoundingClientRect(); return r.width > 20 && Math.abs(r.width - r.height) < 1.5; });
      assert(sq, 'square box layout is still available');
      await desk.click('#layoutStudio [data-v="split"]');
      // resize by dragging the divider between DAW and camera
      const d = await desk.locator('#divStudio').boundingBox();
      await desk.mouse.move(d.x + d.width / 2, d.y + d.height / 2);
      await desk.mouse.down();
      await desk.mouse.move(d.x + d.width / 2 - 120, d.y + d.height / 2, { steps: 6 });
      await desk.mouse.up();
      const ratio2 = await desk.evaluate(() => document.getElementById('preview').getBoundingClientRect().width / document.getElementById('screenMain').getBoundingClientRect().width);
      assert(ratio2 > 0.6, `dragging the divider makes the camera panel bigger (camera/DAW ${ratio2.toFixed(2)})`);
      await desk.fill('#camSizeStudio', '44');
      await desk.dispatchEvent('#camSizeStudio', 'input');
    }
    await desk.click('#recBtn');
    await desk.waitForSelector('#recBadge:not([hidden])');
    await sleep(4000);
    if (shots) { await desk.screenshot({ path: path.join(shots, 'studio.png') }); await phone.screenshot({ path: path.join(shots, 'phone.png') }); }
    await desk.click('#pauseBtn'); await sleep(1500); await desk.click('#pauseBtn');
    // simulate a Wi-Fi drop on the phone mid-recording
    await pctx.setOffline(true);
    await phone.evaluate(() => window.__sockets.forEach((w) => w.close()));
    const dropped = await desk.waitForFunction(() => document.getElementById('chipCam').textContent.includes('Reconnecting'), null, { timeout: 15000 }).then(() => true, () => false);
    assert(dropped, 'desktop shows the camera link dropped while recording continues');
    await sleep(3500); await pctx.setOffline(false);
    await desk.waitForFunction(() => document.getElementById('chipCam').textContent.includes('live'), null, { timeout: 20000 });
    assert(true, 'phone reconnected automatically after a network drop');
    await sleep(3000);
    await desk.click('#recBtn');
    await desk.waitForSelector('#editor:not([hidden])', { timeout: 60000 });
    const id = await desk.evaluate(async () => (await (await fetch('/api/sessions')).json())[0].id);
    const { project } = await (await dctx.request.get(`${server.localUrl}api/sessions/${id}`)).json();
    console.log('  · sync', JSON.stringify(project.sync), 'duration', project.duration.toFixed(2), 'pauses', JSON.stringify(project.timing.pauses));
    assert(project.status === 'ready' && project.duration > 10, `session saved (${project.duration.toFixed(1)} s of audio)`);
    assert(!!project.camera, `camera file saved (${project.camera && project.camera.file})`);
    assert(Math.abs(project.sync.cameraOffsetSec) < 1.5, 'camera start offset is plausible');
    assert(project.timing.pauses.length === 1, 'pause range recorded');
    const dir = path.join(dataDir, 'Sessions', id);
    const probe = (f) => { try { execFileSync(ffmpegPath(), ['-hide_banner', '-i', path.join(dir, f)], { stdio: 'pipe' }); } catch (e) { return String(e.stderr); } return ''; };
    const camDur = /Duration: (\d+):(\d+):([\d.]+)/.exec(probe(project.camera.file));
    const camSec = camDur ? +camDur[1] * 3600 + +camDur[2] * 60 + +camDur[3] : 0;
    assert(Math.abs(camSec - project.duration) < 2.5, `camera video is complete despite the drop (${camSec.toFixed(1)} s vs ${project.duration.toFixed(1)} s audio)`);
    assert(/pcm_s24le.*48000 Hz, (stereo|2 channels)/.test(probe('audio.wav')), 'audio is 24-bit 48 kHz stereo WAV');

    // editor + exports
    await desk.click('#edPlay'); await sleep(1500);
    assert(await desk.evaluate(() => !document.getElementById('edAudio').paused && document.getElementById('edCam').currentTime > 0), 'editor preview plays audio and video together');
    await desk.click('#edPlay');
    const ed = await desk.evaluate(() => {
      const a = document.getElementById('edScreen').getBoundingClientRect(), b = document.querySelector('.ed-cam-wrap').getBoundingClientRect();
      return { layout: document.getElementById('player').dataset.layout, overlap: a.right > b.left + 1 && b.right > a.left + 1, swap: !document.getElementById('swapSides').hidden };
    });
    assert(ed.layout === 'split' && !ed.overlap && ed.swap, 'editor opens with the same two-screen layout');
    await desk.click('#aspectSeg [data-v="16:9"]');
    await desk.selectOption('#qualSel', '720');
    if (shots) { await sleep(400); await desk.screenshot({ path: path.join(shots, 'editor.png') }); }
    await desk.click('#expMp4');
    await desk.waitForFunction(() => document.querySelectorAll('#exportList li').length === 1, null, { timeout: 40000 });
    await desk.click('#expWav');
    await desk.waitForFunction(() => document.querySelectorAll('#exportList li').length === 2, null, { timeout: 60000 });
    const exps = fs.readdirSync(path.join(dir, 'exports'));
    const mp4 = probe(`exports/${exps.find((f) => f.endsWith('.mp4'))}`);
    assert(/Video: h264.*1280x720/.test(mp4) && /Audio: aac/.test(mp4), 'MP4 export is 1280×720 H.264 + AAC');
    if (shots) execFileSync(ffmpegPath(), ['-y', '-loglevel', 'error', '-ss', '2', '-i', path.join(dir, 'exports', exps.find((f) => f.endsWith('.mp4'))), '-frames:v', '1', path.join(shots, 'export-frame.png')]);
    const d = /Duration: \d+:(\d+):([\d.]+)/.exec(mp4);
    const expected = project.duration - (project.timing.pauses[0][1] - project.timing.pauses[0][0]);
    assert(Math.abs(+d[1] * 60 + +d[2] - expected) < 0.3, `export length excludes the pause (${d[2]} s ≈ ${expected.toFixed(2)} s)`);
    assert(/pcm_s24le/.test(probe(`exports/${exps.find((f) => f.endsWith('.wav'))}`)), 'WAV export keeps 24-bit PCM');

    // ── USB camera instead of the phone ──
    await desk.click('.tab[data-view="studio"]');
    await desk.click('#camSrcSeg [data-v="usb"]');
    await desk.waitForFunction(() => document.getElementById('chipCam').textContent.includes('USB camera live'), null, { timeout: 15000 });
    await desk.waitForFunction(() => document.getElementById('preview').videoWidth > 0, null, { timeout: 10000 });
    assert(await desk.evaluate(() => document.getElementById('preview').videoWidth > 0 && document.getElementById('statLink').textContent === 'USB'),
      'USB camera is live in the studio preview');
    await desk.click('#recBtn');
    await desk.waitForSelector('#recBadge:not([hidden])');
    await sleep(5000);
    await desk.click('#recBtn');
    await desk.waitForFunction((prev) => document.querySelector('#sessionList li.active') && document.querySelector('#sessionList li.active').dataset.id !== prev, id, { timeout: 60000 });
    const id2 = await desk.evaluate(() => document.querySelector('#sessionList li.active').dataset.id);
    const p2 = (await (await dctx.request.get(`${server.localUrl}api/sessions/${id2}`)).json()).project;
    assert(!!p2.camera && /\.(mp4|webm)$/.test(p2.camera.file), `USB camera recording saved (${p2.camera && p2.camera.file})`);
    const d2 = /Duration: (\d+):(\d+):([\d.]+)/.exec(probe(`../${id2}/${p2.camera.file}`));
    const s2 = d2 ? +d2[2] * 60 + +d2[3] : 0;
    assert(Math.abs(s2 - p2.duration) < 1.5 && Math.abs(p2.sync.cameraOffsetSec) < 1, `USB camera video matches the audio (${s2.toFixed(1)} s vs ${p2.duration.toFixed(1)} s, offset ${p2.sync.cameraOffsetSec.toFixed(3)} s)`);
  } catch (e) {
    failed = true;
    console.error(e.message);
  }
  await browser.close();
  await server.close();
  if (!process.env.DLC_KEEP) fs.rmSync(dataDir, { recursive: true, force: true }); else console.log('data kept in', dataDir);
  console.log(failed ? '\nE2E FAILED' : '\nE2E PASSED');
  process.exit(failed ? 1 : 0);
})();
