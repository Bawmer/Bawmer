// Audio capture engine: metering, sample-accurate recording windows, and the
// mapping between audio frames and the desktop master clock.

export const clockNow = () => performance.timeOrigin + performance.now();
const KIND = { DAW: 1, MIC: 2 };
const MARKER_INTERVAL_MS = 5000;

export class AudioEngine extends EventTarget {
  constructor() {
    super();
    this.ctx = null;
    this.rec = null;
    this.streams = [];
    this._offsets = []; // recent (arrival − audio time) samples, for the clock mapping
    this.clickWatch = null;
  }

  get running() { return !!this.ctx; }
  get sampleRate() { return this.ctx ? this.ctx.sampleRate : 0; }

  async start({ dawStream, micStream, sampleRate }) {
    await this.stop();
    const ctx = new AudioContext({ sampleRate, latencyHint: 'interactive' });
    await ctx.audioWorklet.addModule('capture-worklet.js');
    this.ctx = ctx;
    this.streams = [dawStream, micStream].filter(Boolean);
    const sink = ctx.createGain();
    sink.gain.value = 0; // pull the graph without ever monitoring (no feedback)
    sink.connect(ctx.destination);
    const tap = (stream, kind) => {
      const node = new AudioWorkletNode(ctx, 'capture', {
        numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2],
        channelCount: 2, channelCountMode: 'explicit', channelInterpretation: 'speakers',
      });
      ctx.createMediaStreamSource(stream).connect(node).connect(sink);
      node.port.onmessage = (e) => this._onBatch(kind, e.data.frame, e.data.samples);
      return node;
    };
    tap(dawStream, KIND.DAW);
    this.hasMic = !!micStream;
    if (micStream) tap(micStream, KIND.MIC);
    if (ctx.state !== 'running') await ctx.resume();
    const track = dawStream.getAudioTracks()[0];
    if (track) track.addEventListener('ended', () => this.dispatchEvent(new Event('lost')));
  }

  async stop() {
    if (!this.ctx) return;
    const ctx = this.ctx;
    this.ctx = null;
    this.rec = null;
    for (const s of this.streams) s.getAudioTracks().forEach((t) => t.stop());
    this.streams = [];
    this._offsets = [];
    await ctx.close().catch(() => {});
  }

  /** Master-clock time (ms) at which the given context frame was captured. */
  frameToClock(frame) {
    let min = Infinity;
    for (const o of this._offsets) if (o < min) min = o;
    return performance.timeOrigin + (frame / this.ctx.sampleRate) * 1000 + min;
  }

  _onBatch(kind, frame, samples) {
    if (!this.ctx) return;
    const frames = samples.length / 2;
    if (kind === KIND.DAW) {
      // Least-delayed arrivals track when audio was really processed; a short
      // window lets the mapping follow slow drift without main-thread jitter.
      this._offsets.push(performance.now() - ((frame + frames) / this.ctx.sampleRate) * 1000);
      if (this._offsets.length > 200) this._offsets.shift();

      let pl = 0, pr = 0, sl = 0, sr = 0;
      for (let i = 0; i < samples.length; i += 2) {
        const l = Math.abs(samples[i]), r = Math.abs(samples[i + 1]);
        if (l > pl) pl = l;
        if (r > pr) pr = r;
        sl += l * l; sr += r * r;
      }
      this.dispatchEvent(new CustomEvent('levels', { detail: {
        peak: [pl, pr], rms: [Math.sqrt(sl / frames), Math.sqrt(sr / frames)],
      } }));

      const cw = this.clickWatch;
      if (cw && frame + frames > cw.fromFrame) {
        for (let i = Math.max(0, cw.fromFrame - frame); i < frames; i++) {
          if (Math.abs(samples[i * 2]) > cw.threshold || Math.abs(samples[i * 2 + 1]) > cw.threshold) {
            this.clickWatch = null;
            cw.resolve(frame + i);
            break;
          }
        }
      }
    }

    const rec = this.rec;
    if (!rec) return;
    const st = rec.tracks[kind];
    if (!st || st.done) return;
    const from = Math.max(frame, rec.startFrame);
    const to = Math.min(frame + frames, rec.stopFrame);
    if (to > from) {
      const part = samples.subarray((from - frame) * 2, (to - frame) * 2);
      const out = new Uint8Array(4 + part.byteLength);
      new DataView(out.buffer).setUint32(0, kind, true);
      out.set(new Uint8Array(part.buffer, part.byteOffset, part.byteLength), 4);
      rec.send(out);
      st.frames += to - from;
      if (kind === KIND.DAW) {
        const now = performance.now();
        if (now - rec.lastMarker >= MARKER_INTERVAL_MS) {
          rec.lastMarker = now;
          rec.markers.push([st.frames, Math.round(this.frameToClock(to) * 100) / 100]);
        }
      }
    }
    if (frame + frames >= rec.stopFrame) {
      st.done = true;
      if (Object.values(rec.tracks).every((t) => t.done)) rec.finished();
    }
  }

  get currentFrame() { return Math.round(this.ctx.currentTime * this.ctx.sampleRate); }

  /** Begin streaming samples to `send`. DAW and mic share one start frame. */
  startRecording(send) {
    const startFrame = this.currentFrame + Math.round(this.ctx.sampleRate * 0.05);
    this.rec = {
      send, startFrame, stopFrame: Infinity, markers: [], lastMarker: performance.now(), finished: () => {},
      tracks: { [KIND.DAW]: { frames: 0, done: false }, ...(this.hasMic ? { [KIND.MIC]: { frames: 0, done: false } } : {}) },
    };
    return { startFrame, startMs: this.frameToClock(startFrame) };
  }

  /** Seconds of audio recorded so far (position on the session timeline). */
  recordedSeconds() {
    if (!this.rec) return 0;
    return Math.max(0, (this.currentFrame - this.rec.startFrame) / this.ctx.sampleRate);
  }

  async stopRecording() {
    const rec = this.rec;
    if (!rec) return null;
    rec.stopFrame = Math.max(this.currentFrame, rec.startFrame + 1);
    await Promise.race([new Promise((r) => { rec.finished = r; }), new Promise((r) => setTimeout(r, 1500))]);
    this.rec = null;
    return { startMs: this.frameToClock(rec.startFrame), markers: rec.markers, frames: rec.tracks[KIND.DAW].frames };
  }

  /** Play a short click and report when it shows up in the captured signal. */
  async measureLoopbackLatency() {
    const ctx = this.ctx;
    const at = ctx.currentTime + 0.08;
    const osc = ctx.createOscillator();
    const g = ctx.createGain();
    osc.frequency.value = 1800;
    g.gain.setValueAtTime(0.9, at);
    g.gain.setValueAtTime(0, at + 0.02);
    osc.connect(g).connect(ctx.destination);
    osc.start(at); osc.stop(at + 0.03);
    const playFrame = Math.round(at * ctx.sampleRate);
    const frame = await new Promise((resolve) => {
      this.clickWatch = { fromFrame: playFrame, threshold: 0.08, resolve };
      setTimeout(() => { this.clickWatch = null; resolve(null); }, 700);
    });
    return frame == null ? null : ((frame - playFrame) / ctx.sampleRate) * 1000;
  }
}
