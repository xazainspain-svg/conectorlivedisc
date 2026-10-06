# conectorlivedisc

Envía el **Master de Ableton Live** a un canal de voz de Discord con la mínima latencia posible.

```
Ableton Master ─► [Live Discord Send VST3] ─UDP localhost─► bridge (Node) ─Opus─► bot en canal de voz
   (passthrough, 0 latencia PDC)          PCM 16-bit, paquetes de 5 ms     ~40 ms prebuffer
```

## Qué es "sin latencia" aquí
- **El plugin añade 0 ms**: es passthrough puro (`setLatencySamples(0)`), no toca el audio ni compensa retardo. La copia al hilo de red es lock-free, nunca bloquea el hilo de audio.
- **El bridge** añade un prebuffer ajustable (`PREBUFFER_MS`, 40 ms por defecto) y descarta audio viejo si se acumula (`MAX_BUFFER_MS`).
- **Discord** añade el suyo (codificación Opus + red + buffer del oyente, típicamente 50–150 ms). Eso no se puede eliminar desde fuera. Ten en cuenta que quien escucha oirá el audio con ese retraso respecto a lo que suena en tu sala.

## 1. Bot de Discord
1. https://discord.com/developers/applications → New Application → Bot → copia el **token**.
2. Invita el bot a tu servidor con permisos *Connect* y *Speak* (OAuth2 → URL Generator → scope `bot`).
3. Activa el modo desarrollador en Discord y copia el ID del servidor (`GUILD_ID`) y del canal de voz (`VOICE_CHANNEL_ID`).

## 2. Bridge
```bash
cd bridge
npm install
cp .env.example .env   # rellena token, guild y canal
node --env-file=.env src/index.js
```
Prueba sin Ableton: `node test/send-test-tone.js` (envía un tono de 440 Hz) y `npm test` para los tests.

## 3. Plugin (VST3)
Requiere CMake ≥ 3.22 y un compilador C++17 (JUCE se descarga solo).
```bash
cd plugin
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
Copia `build/LiveDiscordSend_artefacts/Release/VST3/Live Discord Send.vst3` a tu carpeta VST3
(Windows: `C:\Program Files\Common Files\VST3`, macOS: `/Library/Audio/Plug-Ins/VST3`) y reescanea plugins en Live.
Arrastra el plugin a la **pista Master** (al final de la cadena). Parámetros: `Send to Discord` (on/off) y `UDP Port` (9955, igual que en el bridge).

## Windows (paso a paso)
1. Instala [Node.js 22 LTS](https://nodejs.org).
2. **Plugin**, elige una:
   - *Sin instalar compilador*: en GitHub → pestaña **Actions** → *Build VST3 (Windows)* → *Run workflow*; descarga el artefacto `LiveDiscordSend-vst3-windows`.
   - *Local*: instala Visual Studio 2022 (carga de trabajo "Desarrollo para el escritorio con C++") y CMake; en PowerShell, dentro de `plugin`: `cmake -B build` y `cmake --build build --config Release`.
3. Copia la carpeta `Live Discord Send.vst3` a `C:\Program Files\Common Files\VST3`.
4. En Live: Preferencias → Plug-ins → activa *Use VST3 System Folders* y pulsa *Rescan*. Arrástralo al Master.
5. Haz doble clic en `bridge\start.bat` (la primera vez crea `.env`; rellénalo).
6. Si el Firewall de Windows pregunta por Node, permite acceso privado (el tráfico es solo local, `127.0.0.1`).

## Notas
- Estéreo solamente. Cualquier sample rate: el bridge remuestrea a 48 kHz.
- Cuidado: no pongas el bot en un canal donde tu propio cliente de Discord capture tu salida (feedback).
- Estado: el bridge y el protocolo tienen tests; el plugin JUCE está escrito pero **aún no se ha compilado/probado dentro de Ableton**.
