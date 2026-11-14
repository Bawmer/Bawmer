'use strict';
const { execFile } = require('child_process');

// Process-name fragments used to spot a running DAW, plus where its audio
// output device is chosen (needed when routing through a virtual driver).
const DAWS = [
  { id: 'ableton', name: 'Ableton Live', match: ['ableton live', 'live.exe', '/live'], output: 'Preferences → Audio → Audio Output Device' },
  { id: 'flstudio', name: 'FL Studio', match: ['fl64', 'fl studio', 'fl.exe'], output: 'Options → Audio Settings → Device' },
  { id: 'logic', name: 'Logic Pro', match: ['logic pro'], output: 'Settings → Audio → Devices → Output Device' },
  { id: 'protools', name: 'Pro Tools', match: ['pro tools', 'protools'], output: 'Setup → Playback Engine' },
  { id: 'cubase', name: 'Cubase', match: ['cubase'], output: 'Studio → Studio Setup → Audio System' },
  { id: 'studioone', name: 'Studio One', match: ['studio one'], output: 'Options → Audio Setup → Playback Device' },
  { id: 'reaper', name: 'Reaper', match: ['reaper'], output: 'Preferences → Audio → Device' },
  { id: 'bitwig', name: 'Bitwig Studio', match: ['bitwig'], output: 'Settings → Audio → Output' },
];

function listProcesses() {
  return new Promise((resolve) => {
    const win = process.platform === 'win32';
    execFile(win ? 'tasklist' : 'ps', win ? ['/fo', 'csv', '/nh'] : ['-A', '-o', 'comm='],
      { maxBuffer: 8 << 20, timeout: 4000 }, (err, out) => resolve(err ? '' : String(out).toLowerCase()));
  });
}

async function detectDaws() {
  const procs = await listProcesses();
  return DAWS.map((d) => ({ id: d.id, name: d.name, output: d.output, running: d.match.some((m) => procs.includes(m)) }));
}

module.exports = { detectDaws, DAWS };
