'use strict';
const { app, BrowserWindow, desktopCapturer, session, shell, powerSaveBlocker, systemPreferences } = require('electron');
const path = require('path');
const { start } = require('../server/app');

// LAN-only WebRTC: expose real local addresses so the phone can reach us directly.
app.commandLine.appendSwitch('disable-features', 'WebRtcHideLocalIpsWithMdns');
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');

let win = null;
let server = null;

async function createWindow() {
  if (!server) {
    server = await start({
      dataDir: path.join(app.getPath('videos'), 'DAW Live Cam'),
      electron: true,
      reveal: (target) => shell.openPath(target),
    });
  }

  const ses = session.defaultSession;
  const ours = (url) => typeof url === 'string' && url.startsWith(server.localUrl);
  ses.setPermissionRequestHandler((wc, permission, cb) => cb(ours(wc.getURL()) && ['media', 'display-capture', 'fullscreen'].includes(permission)));
  ses.setPermissionCheckHandler((wc, permission, origin) => ours(origin + '/') && ['media', 'display-capture', 'fullscreen'].includes(permission));
  // Screen capture + system audio: Windows provides WASAPI loopback here. On
  // macOS the audio half is unavailable, so the UI routes through a virtual
  // driver (BlackHole / Loopback) instead and this only supplies screen video.
  ses.setDisplayMediaRequestHandler((request, cb) => {
    desktopCapturer.getSources({ types: ['screen'] }).then((sources) => {
      if (!sources.length) return cb({});
      const streams = { video: sources[0] };
      if (request.audioRequested && process.platform === 'win32') streams.audio = 'loopback';
      cb(streams);
    }).catch(() => cb({}));
  });

  if (process.platform === 'darwin') {
    systemPreferences.askForMediaAccess('microphone').catch(() => {});
  }

  win = new BrowserWindow({
    width: 1440, height: 900, minWidth: 1100, minHeight: 700,
    backgroundColor: '#090a0f', title: 'DAW LIVE CAM', autoHideMenuBar: true,
    webPreferences: { contextIsolation: true, nodeIntegration: false, backgroundThrottling: false },
  });
  win.webContents.setWindowOpenHandler(({ url }) => { if (!ours(url)) shell.openExternal(url); return { action: 'deny' }; });
  win.on('closed', () => { win = null; });
  await win.loadURL(server.localUrl);
}

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => { if (win) { if (win.isMinimized()) win.restore(); win.focus(); } });
  app.whenReady().then(() => {
    powerSaveBlocker.start('prevent-app-suspension'); // long takes must not be throttled
    createWindow();
    app.on('activate', () => { if (!win) createWindow(); });
  });
  app.on('window-all-closed', () => app.quit());
  app.on('before-quit', () => { if (server) server.close(); });
}
