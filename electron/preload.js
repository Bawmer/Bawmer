'use strict';
// Bridges the studio page and the floating control bar to the app shell.
const { contextBridge, ipcRenderer } = require('electron');

const isMini = location.pathname.endsWith('/mini.html');

if (isMini) {
  contextBridge.exposeInMainWorld('dlcMini', {
    onStatus: (cb) => ipcRenderer.on('mini:status', (_e, s) => cb(s)),
    command: (cmd) => ipcRenderer.send('mini:command', String(cmd)),
  });
} else {
  contextBridge.exposeInMainWorld('dlcNative', {
    listSources: () => ipcRenderer.invoke('sources:list'),
    selectSource: (id) => ipcRenderer.invoke('sources:select', id),
    // Busy = recording or live. With autoHide the dashboard minimises and the
    // floating bar takes over; it comes back when nothing is running.
    setBusy: (state) => ipcRenderer.send('studio:busy', state),
    status: (s) => ipcRenderer.send('studio:status', s),
    onCommand: (cb) => ipcRenderer.on('studio:command', (_e, cmd) => cb(cmd)),
  });
}
