'use strict';
// Streaming WAV writer: 24-bit PCM or 32-bit float, switches to RF64 beyond 4 GB.
const fs = require('fs');

const HEADER_BYTES = 80;
const PEAK_SECONDS = 0.05;

class WavWriter {
  constructor(file, { sampleRate, channels = 2, bitDepth = 24 }) {
    this.file = file;
    this.sampleRate = sampleRate;
    this.channels = channels;
    this.bitDepth = bitDepth === 32 ? 32 : 24;
    this.bytesPerSample = this.bitDepth / 8;
    this.fd = fs.openSync(file, 'w');
    this.dataBytes = 0;
    this.frames = 0;
    this.peaks = [];
    this._peakFrames = Math.max(1, Math.round(sampleRate * PEAK_SECONDS));
    this._peakCount = 0;
    this._peakMax = 0;
    this._lastHeaderAt = 0;
    fs.writeSync(this.fd, this._header(), 0, HEADER_BYTES, 0);
  }

  _header() {
    const h = Buffer.alloc(HEADER_BYTES);
    const big = this.dataBytes + HEADER_BYTES - 8 > 0xffffffff;
    h.write(big ? 'RF64' : 'RIFF', 0, 'ascii');
    h.writeUInt32LE(big ? 0xffffffff : this.dataBytes + HEADER_BYTES - 8, 4);
    h.write('WAVE', 8, 'ascii');
    // Reserved chunk: JUNK normally, ds64 when the file outgrows 32-bit sizes.
    h.write(big ? 'ds64' : 'JUNK', 12, 'ascii');
    h.writeUInt32LE(28, 16);
    if (big) {
      h.writeBigUInt64LE(BigInt(this.dataBytes + HEADER_BYTES - 8), 20);
      h.writeBigUInt64LE(BigInt(this.dataBytes), 28);
      h.writeBigUInt64LE(BigInt(this.frames), 36);
      h.writeUInt32LE(0, 44);
    }
    h.write('fmt ', 48, 'ascii');
    h.writeUInt32LE(16, 52);
    h.writeUInt16LE(this.bitDepth === 32 ? 3 : 1, 56); // 3 = IEEE float, 1 = PCM
    h.writeUInt16LE(this.channels, 58);
    h.writeUInt32LE(this.sampleRate, 60);
    h.writeUInt32LE(this.sampleRate * this.channels * this.bytesPerSample, 64);
    h.writeUInt16LE(this.channels * this.bytesPerSample, 68);
    h.writeUInt16LE(this.bitDepth, 70);
    h.write('data', 72, 'ascii');
    h.writeUInt32LE(big ? 0xffffffff : this.dataBytes, 76);
    return h;
  }

  /** @param {Float32Array} samples interleaved */
  write(samples) {
    if (this.fd == null || !samples.length) return;
    const ch = this.channels;
    for (let i = 0; i < samples.length; i++) {
      const a = Math.abs(samples[i]);
      if (a > this._peakMax) this._peakMax = a;
      if (i % ch === ch - 1 && ++this._peakCount >= this._peakFrames) {
        this.peaks.push(Math.round(Math.min(1, this._peakMax) * 1000) / 1000);
        this._peakCount = 0;
        this._peakMax = 0;
      }
    }
    let out;
    if (this.bitDepth === 32) {
      out = Buffer.from(samples.buffer, samples.byteOffset, samples.byteLength);
    } else {
      out = Buffer.allocUnsafe(samples.length * 3);
      for (let i = 0, o = 0; i < samples.length; i++, o += 3) {
        let v = Math.round(Math.max(-1, Math.min(1, samples[i])) * 8388607);
        if (v < 0) v += 16777216;
        out[o] = v & 255;
        out[o + 1] = (v >> 8) & 255;
        out[o + 2] = (v >> 16) & 255;
      }
    }
    fs.writeSync(this.fd, out, 0, out.length, HEADER_BYTES + this.dataBytes);
    this.dataBytes += out.length;
    this.frames += samples.length / ch;
    // Keep the header valid so a crash still leaves a playable file.
    const now = Date.now();
    if (now - this._lastHeaderAt > 5000) {
      this._lastHeaderAt = now;
      fs.writeSync(this.fd, this._header(), 0, HEADER_BYTES, 0);
    }
  }

  close() {
    if (this.fd == null) return;
    if (this._peakCount) this.peaks.push(Math.round(Math.min(1, this._peakMax) * 1000) / 1000);
    fs.writeSync(this.fd, this._header(), 0, HEADER_BYTES, 0);
    fs.closeSync(this.fd);
    this.fd = null;
  }
}

module.exports = { WavWriter, HEADER_BYTES, PEAK_SECONDS };
