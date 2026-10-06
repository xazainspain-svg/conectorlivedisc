// Simulates the plugin: sends a 440 Hz stereo tone as UDP packets in real time.
// Usage: node test/send-test-tone.js [port] [sampleRate]
import dgram from 'node:dgram';
import { buildPacket } from '../src/protocol.js';

const port = Number(process.argv[2] ?? 9955);
const sampleRate = Number(process.argv[3] ?? 44100);
const FR = 240;
const sock = dgram.createSocket('udp4');
let n = 0, seq = 0;
const t0 = process.hrtime.bigint();
setInterval(() => {
  const due = Number((process.hrtime.bigint() - t0) / 1000n) * sampleRate / 1e6; // frames due so far
  while (n + FR <= due) {
    const s = new Int16Array(FR * 2);
    for (let i = 0; i < FR; i++) s[i * 2] = s[i * 2 + 1] = Math.round(Math.sin(2 * Math.PI * 440 * (n + i) / sampleRate) * 8000);
    sock.send(buildPacket({ seq: seq++, sampleRate, samples: s }), port, '127.0.0.1');
    n += FR;
  }
}, 1);
