# Driver de audio virtual propio — Etapa 1 (cable virtual)

Fecha: 2026-10-07 · Estado: pendiente de revisión del usuario

## Objetivo
Que Windows muestre un dispositivo de audio propio ("Live Discord") para llevar el
Master de Ableton a Discord con la mínima latencia, sin depender de VB-CABLE.

## Contexto y decisiones acordadas
- Solo Windows, solo para el PC del autor. Firma: **test-signing** (gratis, marca de agua
  "Test Mode"). La firma de producción (EV + Partner Center) queda como trabajo posterior;
  el diseño no debe impedirla.
- Enfoque por etapas. **Esta spec cubre solo la Etapa 1**: cable virtual con loopback.
  La Etapa 2 (ingesta directa plugin → buffer del kernel, sin motor de audio de Windows)
  se especificará aparte, tras medir la latencia de la Etapa 1.
- El plugin VST3 se mantiene: Ableton usa un solo dispositivo de salida (la interfaz
  ASIO), así que el plugin entrega el Master por UDP sin tocar ese dispositivo.

## Arquitectura
```
Ableton Master ─► plugin VST3 ─UDP 127.0.0.1:9955─► ldsc-device.exe ─WASAPI─► "Live Discord Out"
                                                                                      ⇓ (driver, buffer circular)
                                                                          "Live Discord Mic" ─► Discord
```
- Nueva carpeta `driver/`: driver de audio WDM/PortCls (miniport) derivado de
  Virtual-Audio-Driver (MIT). Se conserva su aviso de licencia en `driver/LICENSE.third-party`.
- Dos endpoints estéreo, 48 kHz, 16 y 24 bits:
  - **Live Discord Out** (reproducción)
  - **Live Discord Mic** (captura)
- El driver solo copia del flujo de reproducción al de captura mediante un buffer circular
  interno. Sin DSP, sin cambio de formato.
- `device/ldsc-device.cpp`: el dispositivo por defecto pasa de `"CABLE Input"` a
  `"Live Discord Out"`; `--device` se mantiene para seguir usando VB-CABLE. Actualizar los
  mensajes de uso/error que mencionan "CABLE Output".

## Compilación e instalación
- Nuevo job `driver` en `.github/workflows/build-plugin.yml` (runner `windows-latest`, WDK vía
  NuGet/instalador) que publica el artefacto `ldsc-driver-windows` (`.sys`, `.inf`, `.cat`).
  Se añade `driver/**` a los `paths` del workflow.
- `driver/install.ps1` (administrador): activa test-signing (avisa de que requiere reinicio),
  crea y confía un certificado de prueba autofirmado, firma el paquete, instala con `pnputil`.
- `driver/uninstall.ps1`: elimina el dispositivo y el paquete del almacén de drivers.
- README: sección nueva con requisitos, instalación, desinstalación, aviso de punto de
  restauración y las advertencias de test-signing (marca de agua, anti-cheat).

## Pruebas
Desde la nube solo se puede comprobar que compila (CI) y revisar el código. La verificación
real la hace el autor en Windows, con esta lista (en el README):
1. Aparecen "Live Discord Out" y "Live Discord Mic" en Sonido de Windows.
2. `ldsc-device` + `bridge/test/send-test-tone.js` → el tono se oye grabando "Live Discord Mic".
3. Discord lista "Live Discord Mic" como micrófono.
4. 10 minutos de audio continuo sin cortes ni deriva.
5. Medición de latencia: clic enviado por el cable y grabado de vuelta; comparar con VB-CABLE.
   Objetivo: buffer del driver ≤ 10 ms (se afinará con la medición).

## Riesgos
- Un fallo en modo kernel puede causar pantallazo azul → crear punto de restauración antes
  de instalar y no tocar los drivers de audio reales.
- Test-signing muestra marca de agua y algunos juegos con anti-cheat no arrancan con él.
- No se puede ejecutar el driver en el entorno de desarrollo (Linux); toda prueba funcional
  depende de que el autor instale y reporte.

## Fuera de alcance (Etapa 1)
Ingesta directa desde el plugin, firma de producción, otros sample rates, más de 2 canales,
interfaz gráfica.

## Criterio de éxito
El autor instala el paquete en su PC, elige "Live Discord Mic" en Discord y oye el Master de
Ableton con una latencia igual o menor que con VB-CABLE.
