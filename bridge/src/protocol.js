// Packet layout (little endian), must match plugin/Source/PluginProcessor.h:
//   u32 magic 'LDSC' | u32 seq | u32 sampleRate | u16 channels | u16 frames | int16[frames*channels]
export const MAGIC = 0x4353444c;
export const HEADER_BYTES = 16;

export function parsePacket(buf) {
  if (buf.length < HEADER_BYTES || buf.readUInt32LE(0) !== MAGIC) return null;
  const channels = buf.readUInt16LE(12);
  const frames = buf.readUInt16LE(14);
  if (channels !== 2 || buf.length < HEADER_BYTES + frames * channels * 2) return null;
  const samples = new Int16Array(frames * channels);
  for (let i = 0; i < samples.length; i++) samples[i] = buf.readInt16LE(HEADER_BYTES + i * 2);
  return { seq: buf.readUInt32LE(4), sampleRate: buf.readUInt32LE(8), channels, frames, samples };
}

export function buildPacket({ seq, sampleRate, samples }) {
  const frames = samples.length / 2;
  const buf = Buffer.alloc(HEADER_BYTES + samples.length * 2);
  buf.writeUInt32LE(MAGIC, 0);
  buf.writeUInt32LE(seq >>> 0, 4);
  buf.writeUInt32LE(sampleRate, 8);
  buf.writeUInt16LE(2, 12);
  buf.writeUInt16LE(frames, 14);
  for (let i = 0; i < samples.length; i++) buf.writeInt16LE(samples[i], HEADER_BYTES + i * 2);
  return buf;
}
