'use strict';
const os = require('os');
const fs = require('fs');
const path = require('path');

function lanAddresses() {
  const out = [];
  for (const [name, addrs] of Object.entries(os.networkInterfaces())) {
    for (const a of addrs || []) {
      if (a.family === 'IPv4' && !a.internal && !a.address.startsWith('169.254.')) out.push({ name, address: a.address });
    }
  }
  // Prefer typical private Wi-Fi/LAN ranges over VPN and virtual adapters.
  const rank = (a) => (a.startsWith('192.168.') ? 0 : a.startsWith('10.') ? 1 : /^172\.(1[6-9]|2\d|3[01])\./.test(a) ? 2 : 3);
  return out.sort((x, y) => rank(x.address) - rank(y.address));
}

/** Self-signed certificate: phones only expose the camera to HTTPS pages. */
function loadCertificate(dataDir) {
  const keyFile = path.join(dataDir, 'tls-key.pem');
  const certFile = path.join(dataDir, 'tls-cert.pem');
  if (fs.existsSync(keyFile) && fs.existsSync(certFile)) {
    return { key: fs.readFileSync(keyFile), cert: fs.readFileSync(certFile) };
  }
  const selfsigned = require('selfsigned');
  const altNames = [{ type: 2, value: 'localhost' }, { type: 7, ip: '127.0.0.1' },
    ...lanAddresses().map((a) => ({ type: 7, ip: a.address }))];
  const pems = selfsigned.generate([{ name: 'commonName', value: 'DAW LIVE CAM' }], {
    days: 3650, keySize: 2048, algorithm: 'sha256',
    extensions: [{ name: 'basicConstraints', cA: false }, { name: 'subjectAltName', altNames }],
  });
  fs.writeFileSync(keyFile, pems.private, { mode: 0o600 });
  fs.writeFileSync(certFile, pems.cert);
  return { key: pems.private, cert: pems.cert };
}

module.exports = { lanAddresses, loadCertificate };
