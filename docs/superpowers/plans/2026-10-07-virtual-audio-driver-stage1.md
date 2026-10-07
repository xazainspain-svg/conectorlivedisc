# Driver de audio virtual — Etapa 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Un driver de audio virtual de Windows con dos endpoints ("Live Discord Out" de reproducción y "Live Discord Mic" de captura) unidos por un buffer circular, más CI, scripts de instalación y ajustes en `ldsc-device`.

**Architecture:** Se vendoriza `VirtualDrivers/Virtual-Audio-Driver` (commit `bb34fba15faf569a6ae9bdea360bc1cf4821354e`) en `driver/`. Ese driver NO hace loopback (su mic emite silencio y su salida graba a archivo), así que se añade `LoopbackRing`, un buffer circular en C++ puro (sin dependencias del kernel, probado en host con g++), y se cablea en `WriteBytes`/`ReadBytes` de `CMiniportWaveRTStream` protegido por spinlock.

**Tech Stack:** C++ (WDK, PortCls/WaveRT, MSBuild), g++ en host para tests del ring, PowerShell, GitHub Actions `windows-latest`.

**Spec:** `docs/superpowers/specs/2026-10-07-virtual-audio-driver-stage1-design.md`

## Global Constraints
- Solo Windows x64 (se elimina ARM64 del CI y de los scripts).
- Endpoints estéreo, 48 kHz, 16 y 24 bits; nombres exactos `Live Discord Out` y `Live Discord Mic`.
- Firma de prueba (test-signing) únicamente; nada de firma de producción.
- Conservar licencias: `driver/LICENSE` (MIT, MikeTheTech) y `driver/THIRD_PARTY_NOTICES.md` (MS-PL, Microsoft).
- Buffer del driver objetivo ≤ 10 ms (`LoopbackRing` por defecto: capacidad 40 ms, prebuffer 10 ms).
- Desde Linux solo se puede verificar: tests del ring con g++ y compilación en el CI de Windows. La prueba funcional la hace el autor.
- Mensajes de usuario en español, como el resto del repo.

## Review Focus
- Captura activa sin reproducción activa → silencio, nunca audio viejo repetido.
- Reproducción activa sin captura → no se acumula ni bloquea; el ring descarta lo más antiguo.
- Reinicio de stream (Stop/Run) → el ring se vacía (flush), no suenan restos.
- Formatos distintos entre endpoints (16 vs 24 bits) → el segundo que abre se rechaza o convierte; nunca ruido.
- Underrun (el lector pide más de lo que hay) → rellena con ceros, no repite ni se bloquea; overrun → descarta lo más antiguo hasta mantener ≤ capacidad.

---

### Task 1: Vendorizar el driver base y renombrar los endpoints

**Files:**
- Create: `driver/` (copia de `Source/`, `Package/`, `VirtualAudioDriver.sln`, `LICENSE`, `THIRD_PARTY_NOTICES.md` del commit `bb34fba`), `driver/UPSTREAM.md`
- Modify: `driver/Source/Main/VirtualAudioDriver.inx`, `driver/Source/Filters/*.h`/`*.cpp` donde aparezcan los nombres de endpoint

**Interfaces:**
- Produces: árbol `driver/` que compila con `msbuild driver/VirtualAudioDriver.sln /p:Configuration=Release /p:Platform=x64`; endpoints con nombres `Live Discord Out` y `Live Discord Mic`.

- [ ] **Step 1:** Copiar los archivos listados desde `/home/user/virtualdrivers/virtual-audio-driver` (sin `.git`, `.github`, `build.bat`) a `driver/`. Si ese clon no existe, `git clone --depth 1` de `https://github.com/VirtualDrivers/virtual-audio-driver` y comprobar que `git rev-parse HEAD` sea `bb34fba15faf569a6ae9bdea360bc1cf4821354e`.
- [ ] **Step 2:** Crear `driver/UPSTREAM.md` con URL, commit, licencias (MIT + MS-PL) y la lista de archivos que este repo modificó (se mantiene al día en tareas siguientes).
- [ ] **Step 3:** Quitar la plataforma ARM64 de `VirtualAudioDriver.sln` y de los `.vcxproj` (`grep -rn ARM64 driver/` no devuelve nada).
- [ ] **Step 4:** Renombrar en `VirtualAudioDriver.inx` y en las tablas de topología los nombres de dispositivo y de endpoint a `Live Discord Out` (speaker) y `Live Discord Mic` (micrófono). Verificar: `grep -rn "Virtual Mic Driver\|Virtual Audio Driver" driver/Source` sin resultados salvo comentarios de copyright.
- [ ] **Step 5:** Commit `feat(driver): vendor Virtual-Audio-Driver base and rename endpoints`.

### Task 2: `LoopbackRing` con tests en host

**Files:**
- Create: `driver/Source/Utilities/loopbackring.h`, `driver/Source/Utilities/loopbackring.cpp`, `driver/tests/loopbackring_test.cpp`, `driver/tests/run.sh`

**Interfaces:**
- Produces (namespace `ldsc`, C++ puro, sin `#include <ntddk.h>`; el almacenamiento lo aporta quien lo construye):
  - `class LoopbackRing { public: LoopbackRing(uint8_t* storage, uint32_t capacityBytes, uint32_t prebufferBytes); void Reset(); uint32_t Write(const uint8_t* src, uint32_t bytes); uint32_t Read(uint8_t* dst, uint32_t bytes); uint32_t Available() const; };`
  - `Write` nunca falla: si no cabe, descarta lo más antiguo y devuelve los bytes escritos (= `bytes`). `Read` siempre rellena `bytes` en `dst` y devuelve los bytes reales leídos; el resto lo rellena con ceros. Hasta que `Available() >= prebufferBytes` por primera vez tras `Reset`, `Read` entrega solo ceros (devuelve 0).

- [ ] **Step 1: Write the failing tests** en `loopbackring_test.cpp` (sin framework: `assert` y `main`). Casos, con ring de capacidad 16 bytes y prebuffer 4:
  - `reads_zeros_before_prebuffer`: escribir 3 bytes, leer 3 → devuelve 0 y `dst` todo ceros.
  - `delivers_in_order_after_prebuffer`: escribir `1..8`, leer 8 → devuelve 8 y `1..8`.
  - `underrun_pads_with_zeros_without_repeating`: escribir `1..4`, leer 6 → devuelve 4, `dst == {1,2,3,4,0,0}`; leer 4 más → todo ceros.
  - `overrun_drops_oldest`: escribir 24 bytes `1..24` en capacidad 16 → `Available()==16` y leer 16 da `9..24`.
  - `wraps_correctly`: tres rondas de escribir 10 / leer 10 con capacidad 16 mantienen el orden.
  - `reset_flushes`: tras `Reset()`, `Available()==0` y se vuelve a exigir prebuffer.
- [ ] **Step 2:** Crear `run.sh` (`g++ -std=c++17 -Wall -Wextra -I../Source/Utilities loopbackring_test.cpp ../Source/Utilities/loopbackring.cpp -o /tmp/lr_test && /tmp/lr_test`). Ejecutarlo: Expected: error de compilación (faltan archivos).
- [ ] **Step 3:** Implementar `LoopbackRing` en `loopbackring.{h,cpp}` con índices de lectura/escritura y contador de bytes disponibles; un `bool primed_` que se activa al alcanzar `prebufferBytes` y se mantiene hasta `Reset()`.
- [ ] **Step 4:** Ejecutar `bash driver/tests/run.sh`. Expected: imprime `OK` y sale con código 0.
- [ ] **Step 5:** Commit `feat(driver): add LoopbackRing with host tests`.

### Task 3: Cablear el ring en los streams del driver

**Files:**
- Modify: `driver/Source/Main/minwavertstream.cpp` (`WriteBytes`, `ReadBytes`, `Init`/`Stop`/`SetState`), `driver/Source/Main/minwavertstream.h`, `driver/Source/Main/common.cpp` (inicialización global), `driver/Source/Main/Main.vcxproj` (añadir `loopbackring.cpp`)
- Modify: `driver/UPSTREAM.md`

**Interfaces:**
- Consumes: `ldsc::LoopbackRing` de la Tarea 2.
- Produces: instancia global única `g_LoopbackRing` + `KSPIN_LOCK g_LoopbackLock`, con `LoopbackInit()`/`LoopbackShutdown()` llamadas desde el arranque/parada del driver; almacenamiento en `NonPagedPoolNx` de 40 ms a 48 kHz, estéreo, 24 bits (`48 * 40 * 6 = 11520` bytes máx.).

- [ ] **Step 1:** En `CMiniportWaveRTStream::ReadBytes` (stream de reproducción) sustituir `m_SaveData.WriteData(...)` por `g_LoopbackRing.Write(...)` bajo `KeAcquireSpinLockAtDpcLevel(&g_LoopbackLock)`. Mantener `g_DoNotCreateDataFiles = TRUE` para que no se cree ningún archivo.
- [ ] **Step 2:** En `WriteBytes` (stream de captura) sustituir `RtlZeroMemory` por `g_LoopbackRing.Read(...)` bajo el mismo lock (el ring ya rellena con ceros en underrun).
- [ ] **Step 3:** Al pasar un stream a `KSSTATE_STOP`/`ACQUIRE` y en `Init`, llamar a `g_LoopbackRing.Reset()` bajo lock cuando el stream sea el de reproducción, para que no queden restos al reiniciar.
- [ ] **Step 4:** Formato: en `speakerwavtable.h` y `micarraywavtable.h` dejar solo 48 kHz, 2 canales, 16 y 24 bits. Si el stream de captura abre con distinto ancho de bit que el de reproducción activo, `Init` devuelve `STATUS_INVALID_DEVICE_REQUEST` (decisión: rechazar en vez de convertir; se documenta en el README).
- [ ] **Step 5:** Añadir `loopbackring.cpp` a `Main.vcxproj`. Documentar en `UPSTREAM.md` los archivos modificados.
- [ ] **Step 6:** Revisión adversarial del diff: ningún `PAGED_CODE` en rutas llamadas a DISPATCH_LEVEL; el lock siempre se libera; sin asignaciones en la ruta de audio. (La compilación real se comprueba en la Tarea 5.)
- [ ] **Step 7:** Commit `feat(driver): loopback render stream into capture stream via LoopbackRing`.

### Task 4: Scripts de instalación y desinstalación

**Files:**
- Create: `driver/install.ps1`, `driver/uninstall.ps1`

**Interfaces:**
- Produces: `install.ps1 [-PackageDir <ruta>]` (por defecto, la carpeta del script; exige administrador) y `uninstall.ps1`.

- [ ] **Step 1:** `install.ps1`: comprobar administrador; si test-signing está apagado (`bcdedit /enum {current}`), activarlo con `bcdedit /set testsigning on` y avisar con un mensaje claro de que hay que reiniciar y volver a ejecutar; crear (si no existe) un certificado de código autofirmado `CN=LiveDiscordTest`, importarlo a `Root` y `TrustedPublisher` del equipo; firmar `.sys` y `.cat` con `signtool` (buscarlo en `Windows Kits\10\bin\*\x64`); instalar con `pnputil /add-driver <inf> /install` y crear el dispositivo raíz con `devcon`/`pnputil` según lo que el INF requiera (documentarlo en el script).
- [ ] **Step 2:** `uninstall.ps1`: localizar el dispositivo por hardware ID y quitarlo; `pnputil /delete-driver <oem#.inf> /uninstall /force`; dejar test-signing como está y decirlo en el mensaje final.
- [ ] **Step 3:** Verificar sintaxis desde Linux si hay `pwsh`; si no, el CI de la Tarea 5 ejecuta `Invoke-ScriptAnalyzer` o al menos `[scriptblock]::Create((Get-Content ...))` sobre ambos. Commit `feat(driver): add install and uninstall scripts`.

### Task 5: CI de compilación y tests del ring

**Files:**
- Modify: `.github/workflows/build-plugin.yml`

- [ ] **Step 1:** Añadir `driver/**` a `paths`. Añadir job `ring-tests` (`ubuntu-latest`, `bash driver/tests/run.sh`).
- [ ] **Step 2:** Añadir job `driver` (`windows-latest`, `matrix` solo `x64`): instalar WDK (como hace el workflow upstream, `choco install windowsdriverkit11 -y`), `microsoft/setup-msbuild`, `msbuild driver/VirtualAudioDriver.sln /p:Configuration=Release /p:Platform=x64`, validar scripts PowerShell, y subir el artefacto `ldsc-driver-windows` con `.sys`, `.inf`, `.cat` y los dos `.ps1`.
- [ ] **Step 3:** Push y comprobar los resultados del CI con las herramientas de GitHub. Si el job `driver` falla, corregir con el log (`get_job_logs`) y volver a subir hasta que quede en verde. Expected: `ring-tests` y `driver` en verde, artefacto presente.
- [ ] **Step 4:** Commit `ci: build driver and run ring tests`.

### Task 6: Ajustes en `ldsc-device`

**Files:**
- Modify: `device/ldsc-device.cpp:83` (dispositivo por defecto), `:118`, `:179` (mensajes)

- [ ] **Step 1:** `Config::device` por defecto `L"Live Discord Out"`; si no se encuentra, probar `L"CABLE Input"` antes de fallar (conserva compatibilidad con VB-CABLE).
- [ ] **Step 2:** Actualizar los mensajes de ayuda y de error: mencionar `Live Discord Mic` como micrófono en Discord y la opción `--device` para VB-CABLE.
- [ ] **Step 3:** Verificar que el CI del job `device` sigue verde. Commit `feat(device): default to Live Discord Out with VB-CABLE fallback`.

### Task 7: README y lista de pruebas

**Files:**
- Modify: `README.md`

- [ ] **Step 1:** Nueva sección "Driver propio (Live Discord)": requisitos, instalación con `install.ps1`, desinstalación, aviso de punto de restauración, advertencias de test-signing (marca de agua, anti-cheat) y la limitación de formato (16/24 bits a 48 kHz, ambos endpoints iguales).
- [ ] **Step 2:** Añadir la lista de pruebas de la spec (5 puntos) como casillas marcables, con el método de medición de latencia por clic.
- [ ] **Step 3:** Actualizar la línea "Estado" del final para reflejar que el driver compila en CI pero no se ha probado en una máquina real. Commit `docs: document custom virtual audio driver`.

---

## Self-review
- Cobertura de la spec: arquitectura (T1–T3), compilación/instalación (T4–T5), `ldsc-device` (T6), pruebas/README (T7), riesgos (T3 paso 6, T7). Dos desviaciones respecto a la spec, a registrar en ella: el driver base trae licencia MS-PL además de MIT, y el loopback no existe en el driver base y se implementa aquí.
- Consistencia de tipos: `LoopbackRing` (T2) se usa con el mismo nombre y firma en T3.
- Proporción: el plan fija nombres, firmas, valores y casos de prueba; los cuerpos los decide quien implementa.
