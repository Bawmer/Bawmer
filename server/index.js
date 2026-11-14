#!/usr/bin/env node
'use strict';
// Runs the DAW LIVE CAM engine without the Electron shell; open the printed
// URL in Chrome or Edge on this computer.
const { start } = require('./app');

start().then((s) => {
  console.log(`\n  DAW LIVE CAM\n  Desktop studio : ${s.localUrl}`);
  for (const u of s.pairUrls()) console.log(`  Phone (${u.name}) : ${u.url}`);
  console.log(`  Recordings     : ${s.dataDir}\n`);
}).catch((e) => { console.error(e); process.exit(1); });
