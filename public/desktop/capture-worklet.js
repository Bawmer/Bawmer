// Collects stereo input into interleaved Float32 batches, tagged with the
// AudioContext frame of their first sample so recordings are sample-accurate.
const BATCH = 2048;

class Capture extends AudioWorkletProcessor {
  constructor() {
    super();
    this.buf = new Float32Array(BATCH * 2);
    this.fill = 0;
    this.startFrame = 0;
  }
  process(inputs) {
    const input = inputs[0];
    const l = input && input[0];
    const r = input && (input[1] || input[0]);
    const n = l ? l.length : 128;
    if (this.fill === 0) this.startFrame = currentFrame;
    for (let i = 0; i < n; i++) {
      this.buf[this.fill * 2] = l ? l[i] : 0;
      this.buf[this.fill * 2 + 1] = r ? r[i] : 0;
      if (++this.fill === BATCH) {
        this.port.postMessage({ frame: this.startFrame, samples: this.buf }, [this.buf.buffer]);
        this.buf = new Float32Array(BATCH * 2);
        this.fill = 0;
        this.startFrame = currentFrame + i + 1;
      }
    }
    return true;
  }
}
registerProcessor('capture', Capture);
