'use strict';
const { app, BrowserWindow, desktopCapturer, ipcMain, session, shell, powerSaveBlocker, systemPreferences } = require('electron');
const path = require('path');
const { start } = require('../server/app');

// LAN-only WebRTC: expose real local addresses so the phone can reach us directly.
app.commandLine.appendSwitch('disable-features', 'WebRtcHideLocalIpsWithMdns');
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');

let win = null;
let server = null;
let selectedSource = null; // desktopCapturer id chosen in the studio (DAW window or screen)
const OWN_TITLE = 'DAW LIVE CAM';

function fromStudio(event) {
  return !!server && !!event.senderFrame && event.senderFrame.url.startsWith(server.localUrl);
}

ipcMain.handle('sources:list', async (event) => {
  if (!fromStudio(event)) return [];
  const sources = await desktopCapturer.getSources({ types: ['window', 'screen'], thumbnailSize: { width: 400, height: 250 } });
  return sources
    // Our own window is pointless to record; minimised windows have no picture.
    .filter((s) => s.name !== OWN_TITLE && !s.thumbnail.isEmpty())
    .sort((a, b) => (a.id.startsWith('screen:') ? 0 : 1) - (b.id.startsWith('screen:') ? 0 : 1))
    .map((s) => ({ id: s.id, name: s.name, kind: s.id.startsWith('screen:') ? 'screen' : 'window', thumb: s.thumbnail.toDataURL() }));
});
ipcMain.handle('sources:select', (event, id) => {
  if (fromStudio(event)) selectedSource = typeof id === 'string' ? id : null;
  return true;
});

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
    desktopCapturer.getSources({ types: ['screen', 'window'] }).then((sources) => {
      const pick = sources.find((s) => s.id === selectedSource) || sources.find((s) => s.id.startsWith('screen:'));
      if (!pick) return cb({});
      const streams = { video: pick };
      if (request.audioRequested && process.platform === 'win32') streams.audio = 'loopback';
      cb(streams);
    }).catch(() => cb({}));
  });

  if (process.platform === 'darwin') {
    systemPreferences.askForMediaAccess('microphone').catch(() => {});
    systemPreferences.askForMediaAccess('camera').catch(() => {});
  }

  win = new BrowserWindow({
    width: 1440, height: 900, minWidth: 1100, minHeight: 700,
    backgroundColor: '#090a0f', title: OWN_TITLE, autoHideMenuBar: true,
    webPreferences: { contextIsolation: true, nodeIntegration: false, backgroundThrottling: false, preload: path.join(__dirname, 'preload.js') },
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
