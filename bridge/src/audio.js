// Streaming linear resampler for interleaved stereo Int16 -> 48 kHz (Discord's rate).
// Works on a virtual stream [prevFrame, ...input]; `pos` is a fractional index into it.
export class StereoResampler {
  constructor(outRate = 48000) {
    this.outRate = outRate;
    this.inRate = outRate;
    this.pos = 0;
    this.prev = null;
  }

  process(input, inRate) {
    if (inRate === this.outRate) return input;
    if (inRate !== this.inRate) { this.inRate = inRate; this.pos = 0; this.prev = null; }
    const frames = input.length / 2;
    if (frames === 0) return new Int16Array(0);
    if (!this.prev) this.prev = [input[0], input[1]];
    const n = frames + 1;
    const at = (i, ch) => (i === 0 ? this.prev[ch] : input[(i - 1) * 2 + ch]);
    const step = inRate / this.outRate;
    const out = [];
    let pos = this.pos;
    while (Math.floor(pos) + 1 < n) {
      const i = Math.floor(pos);
      const f = pos - i;
      for (let ch = 0; ch < 2; ch++) {
        const a = at(i, ch);
        out.push(Math.round(a + (at(i + 1, ch) - a) * f));
      }
      pos += step;
    }
    this.pos = pos - (n - 1); // last input frame becomes index 0 next call
    this.prev = [input[(frames - 1) * 2], input[(frames - 1) * 2 + 1]];
    return Int16Array.from(out);
  }
}

// Bounded PCM FIFO (48 kHz stereo Int16). Keeps latency low by dropping the oldest
// audio when the sender runs ahead, and outputs silence on underrun.
export class PcmFifo {
  constructor({ targetFrames = 1920, maxFrames = 4800 } = {}) {
    this.target = targetFrames; // ~40 ms prebuffer
    this.max = maxFrames;       // ~100 ms hard cap
    this.chunks = [];
    this.frames = 0;
    this.primed = false;
    this.dropped = 0;
    this.underruns = 0;
  }

  push(samples) {
    this.chunks.push(samples);
    this.frames += samples.length / 2;
    while (this.frames > this.max && this.chunks.length > 1) {
      const c = this.chunks.shift();
      this.frames -= c.length / 2;
      this.dropped += c.length / 2;
    }
  }

  // Always returns exactly `frames` frames (silence-padded).
  read(frames) {
    const out = new Int16Array(frames * 2);
    if (!this.primed) {
      if (this.frames < this.target) return out;
      this.primed = true;
    }
    let w = 0;
    while (w < out.length && this.chunks.length) {
      const c = this.chunks[0];
      const n = Math.min(c.length, out.length - w);
      out.set(c.subarray(0, n), w);
      w += n;
      if (n === c.length) this.chunks.shift(); else this.chunks[0] = c.subarray(n);
    }
    this.frames -= w / 2;
    if (w < out.length) { this.underruns++; this.primed = false; }
    return out;
  }
}
