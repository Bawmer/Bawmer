'use strict';
const { app, BrowserWindow, desktopCapturer, globalShortcut, ipcMain, screen, session, shell, powerSaveBlocker, systemPreferences } = require('electron');
const path = require('path');
const { start } = require('../server/app');

// LAN-only WebRTC: expose real local addresses so the phone can reach us directly.
app.commandLine.appendSwitch('disable-features', 'WebRtcHideLocalIpsWithMdns');
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
// Keep capturing, compositing and streaming at full rate while the dashboard is minimised.
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('disable-backgrounding-occluded-windows');

let win = null;
let server = null;
let mini = null; // floating control bar shown while the dashboard is hidden
let busy = null; // { recording, live, autoHide }
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
function showMini() {
  if (mini && !mini.isDestroyed()) { mini.showInactive(); return; }
  const area = screen.getPrimaryDisplay().workArea;
  mini = new BrowserWindow({
    width: 300, height: 56, x: Math.round(area.x + area.width / 2 - 150), y: area.y + 12,
    frame: false, resizable: false, alwaysOnTop: true, skipTaskbar: true, focusable: true, show: false,
    transparent: true, hasShadow: false, title: 'DAW LIVE CAM controls',
    webPreferences: { contextIsolation: true, nodeIntegration: false, preload: path.join(__dirname, 'preload.js') },
  });
  mini.setAlwaysOnTop(true, 'screen-saver');
  mini.setContentProtection(true); // never appears in the screen capture or the stream
  mini.loadURL(server.localUrl + 'mini.html');
  mini.once('ready-to-show', () => mini && mini.showInactive());
  mini.on('closed', () => { mini = null; });
}
function hideMini() { if (mini && !mini.isDestroyed()) mini.hide(); }

function restoreDashboard() {
  if (!win) return;
  if (win.isMinimized()) win.restore();
  win.show();
  win.focus();
}

ipcMain.on('studio:busy', (event, state) => {
  if (!fromStudio(event)) return;
  const was = busy && (busy.recording || busy.live);
  busy = state || null;
  const now = busy && (busy.recording || busy.live);
  if (now && busy.autoHide) {
    showMini();
    if (!was && win) win.minimize();
  } else if (!now) {
    hideMini();
    if (was) restoreDashboard();
  }
});
ipcMain.on('studio:status', (event, s) => {
  if (fromStudio(event) && mini && !mini.isDestroyed()) mini.webContents.send('mini:status', s);
});
ipcMain.on('mini:command', (event, cmd) => {
  if (!mini || event.sender !== mini.webContents) return;
  if (cmd === 'show') { restoreDashboard(); return; }
  if (win && ['stop', 'pause', 'live-stop'].includes(cmd)) win.webContents.send('studio:command', cmd);
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
  win.on('closed', () => { win = null; if (mini && !mini.isDestroyed()) mini.destroy(); app.quit(); });
  await win.loadURL(server.localUrl);
}

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => { if (win) { if (win.isMinimized()) win.restore(); win.focus(); } });
  app.whenReady().then(() => {
    powerSaveBlocker.start('prevent-app-suspension'); // long takes must not be throttled
    createWindow();
    // Work from inside the DAW: Ctrl+Alt+R record/stop, Ctrl+Alt+P pause, Ctrl+Alt+D dashboard.
    const shortcut = (keys, cmd) => { try { globalShortcut.register(keys, () => (cmd === 'show' ? restoreDashboard() : win && win.webContents.send('studio:command', cmd))); } catch { /* taken */ } };
    shortcut('CommandOrControl+Alt+R', 'toggle');
    shortcut('CommandOrControl+Alt+P', 'pause');
    shortcut('CommandOrControl+Alt+D', 'show');
    app.on('activate', () => { if (!win) createWindow(); });
  });
  app.on('will-quit', () => globalShortcut.unregisterAll());
  app.on('window-all-closed', () => app.quit());
  app.on('before-quit', () => { if (server) server.close(); });
}
