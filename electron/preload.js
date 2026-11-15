'use strict';
// Lets the studio page choose what to capture (a DAW window or a screen).
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('dlcNative', {
  listSources: () => ipcRenderer.invoke('sources:list'),
  selectSource: (id) => ipcRenderer.invoke('sources:select', id),
});
