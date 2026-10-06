import test from 'node:test';
import assert from 'node:assert/strict';
import { buildPacket, parsePacket } from '../src/protocol.js';
import { StereoResampler, PcmFifo } from '../src/audio.js';

test('packet roundtrip', () => {
  const samples = Int16Array.from([1, -2, 300, -400, 32767, -32768]);
  const p = parsePacket(buildPacket({ seq: 7, sampleRate: 44100, samples }));
  assert.equal(p.seq, 7); assert.equal(p.sampleRate, 44100); assert.equal(p.frames, 3);
  assert.deepEqual([...p.samples], [...samples]);
});

test('rejects bad packets', () => {
  assert.equal(parsePacket(Buffer.alloc(40)), null);
  assert.equal(parsePacket(Buffer.alloc(4)), null);
});

test('resampler 44.1k->48k keeps ratio and is continuous across chunks', () => {
  const r = new StereoResampler(48000);
  const total = 44100; // 1 second of a ramp
  const all = new Int16Array(total * 2);
  for (let i = 0; i < total; i++) { all[i * 2] = i % 20000; all[i * 2 + 1] = i % 20000; }
  const parts = [];
  for (let off = 0; off < total; off += 240) parts.push(r.process(all.subarray(off * 2, Math.min(total, off + 240) * 2), 44100));
  const out = Int16Array.from(parts.flatMap((p) => [...p]));
  assert.ok(Math.abs(out.length / 2 - 48000) < 5, `got ${out.length / 2}`);
  // chunked result must match one-shot result (up to float rounding of +-1 LSB)
  const one = new StereoResampler(48000).process(all, 44100);
  for (let i = 0; i < one.length - 4; i++) assert.ok(Math.abs(out[i] - one[i]) <= 1, `diff at ${i}`);
});

test('fifo: prebuffer, silence on underrun, drops oldest when too full', () => {
  const f = new PcmFifo({ targetFrames: 100, maxFrames: 300 });
  f.push(new Int16Array(100).fill(5)); // 50 frames < target
  assert.ok(f.read(10).every((x) => x === 0));
  f.push(new Int16Array(100).fill(5)); // now 100 frames
  assert.ok(f.read(10).every((x) => x === 5));
  for (let i = 0; i < 10; i++) f.push(new Int16Array(100).fill(9));
  assert.ok(f.frames <= 300 && f.dropped > 0);
});
