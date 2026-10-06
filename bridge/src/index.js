import dgram from 'node:dgram';
import { Readable } from 'node:stream';
import { Client, GatewayIntentBits } from 'discord.js';
import {
  joinVoiceChannel, createAudioPlayer, createAudioResource,
  NoSubscriberBehavior, StreamType, VoiceConnectionStatus, entersState,
} from '@discordjs/voice';
import { parsePacket } from './protocol.js';
import { StereoResampler, PcmFifo } from './audio.js';

const {
  DISCORD_TOKEN, GUILD_ID, VOICE_CHANNEL_ID,
  UDP_PORT = '9955', PREBUFFER_MS = '40', MAX_BUFFER_MS = '100',
} = process.env;

for (const [k, v] of Object.entries({ DISCORD_TOKEN, GUILD_ID, VOICE_CHANNEL_ID })) {
  if (!v) { console.error(`Falta la variable de entorno ${k} (ver .env.example)`); process.exit(1); }
}

const FRAME_MS = 20;
const FRAMES_PER_20MS = 48000 * FRAME_MS / 1000; // 960
const fifo = new PcmFifo({
  targetFrames: Math.round(48000 * Number(PREBUFFER_MS) / 1000),
  maxFrames: Math.round(48000 * Number(MAX_BUFFER_MS) / 1000),
});
const resampler = new StereoResampler(48000);

// --- UDP input from the plugin ---
const udp = dgram.createSocket('udp4');
let lastSeq = null, lost = 0;
udp.on('message', (msg) => {
  const p = parsePacket(msg);
  if (!p) return;
  if (lastSeq !== null && p.seq !== ((lastSeq + 1) >>> 0)) lost++;
  lastSeq = p.seq;
  fifo.push(resampler.process(p.samples, p.sampleRate));
});
udp.bind(Number(UDP_PORT), '127.0.0.1', () => console.log(`Escuchando al plugin en udp://127.0.0.1:${UDP_PORT}`));

// --- Pull-based PCM stream: one 20 ms frame per read, never reads ahead ---
const pcm = new Readable({
  highWaterMark: FRAMES_PER_20MS * 4,
  read() {
    const f = fifo.read(FRAMES_PER_20MS);
    this.push(Buffer.from(f.buffer, f.byteOffset, f.byteLength));
  },
});

// --- Discord ---
const client = new Client({ intents: [GatewayIntentBits.Guilds, GatewayIntentBits.GuildVoiceStates] });
const player = createAudioPlayer({ behaviors: { noSubscriber: NoSubscriberBehavior.Play } });

client.once('clientReady', async () => {
  const guild = await client.guilds.fetch(GUILD_ID);
  const connection = joinVoiceChannel({
    channelId: VOICE_CHANNEL_ID, guildId: GUILD_ID,
    adapterCreator: guild.voiceAdapterCreator, selfDeaf: true,
  });
  connection.on(VoiceConnectionStatus.Disconnected, async () => {
    try {
      await Promise.race([
        entersState(connection, VoiceConnectionStatus.Signalling, 5000),
        entersState(connection, VoiceConnectionStatus.Connecting, 5000),
      ]);
    } catch { connection.destroy(); }
  });
  await entersState(connection, VoiceConnectionStatus.Ready, 30_000);
  connection.subscribe(player);
  player.play(createAudioResource(pcm, { inputType: StreamType.Raw }));
  console.log('Transmitiendo el master a Discord.');
});

setInterval(() => {
  if (lost || fifo.dropped || fifo.underruns)
    console.log(`paquetes perdidos=${lost} descartados=${fifo.dropped}fr underruns=${fifo.underruns}`);
}, 5000).unref();

client.login(DISCORD_TOKEN);
