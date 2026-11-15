'use strict';
const fs = require('fs');
const path = require('path');

class Store {
  constructor(dataDir) {
    this.dataDir = dataDir;
    this.sessionsDir = path.join(dataDir, 'Sessions');
    fs.mkdirSync(this.sessionsDir, { recursive: true });
    this.settingsFile = path.join(dataDir, 'settings.json');
  }

  settings() {
    try { return JSON.parse(fs.readFileSync(this.settingsFile, 'utf8')); } catch { return {}; }
  }
  saveSettings(patch) {
    const s = { ...this.settings(), ...patch };
    fs.writeFileSync(this.settingsFile, JSON.stringify(s, null, 2));
    return s;
  }

  static validId(id) { return typeof id === 'string' && /^[A-Za-z0-9_-]{6,64}$/.test(id); }
  dir(id) {
    if (!Store.validId(id)) throw new Error('Invalid session id');
    return path.join(this.sessionsDir, id);
  }

  create(meta) {
    const d = new Date();
    const p = (n) => String(n).padStart(2, '0');
    const id = `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}-${Math.random().toString(36).slice(2, 6)}`;
    fs.mkdirSync(path.join(this.dir(id), 'exports'), { recursive: true });
    const project = { id, createdAt: d.toISOString(), status: 'recording', exports: [], ...meta };
    this.save(project);
    return project;
  }
  save(project) {
    const file = path.join(this.dir(project.id), 'project.json');
    fs.writeFileSync(file + '.tmp', JSON.stringify(project, null, 2));
    fs.renameSync(file + '.tmp', file);
  }
  load(id) {
    return JSON.parse(fs.readFileSync(path.join(this.dir(id), 'project.json'), 'utf8'));
  }
  list() {
    const out = [];
    for (const id of fs.readdirSync(this.sessionsDir)) {
      try {
        const p = this.load(id);
        out.push({ id: p.id, title: p.title, mode: p.mode, createdAt: p.createdAt, duration: p.duration,
          status: p.status, hasCamera: !!p.camera, hasCamera2: !!p.camera2, hasScreen: !!p.screen, audio: p.audio, exports: (p.exports || []).length });
      } catch { /* not a session folder */ }
    }
    return out.sort((a, b) => (a.createdAt < b.createdAt ? 1 : -1));
  }
  remove(id) {
    fs.rmSync(this.dir(id), { recursive: true, force: true });
  }
  async storage() {
    try {
      const s = await fs.promises.statfs(this.dataDir);
      return { free: s.bavail * s.bsize, total: s.blocks * s.bsize };
    } catch { return { free: null, total: null }; }
  }
}

module.exports = { Store };
