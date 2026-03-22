# BYT3 — Bytebeat Machine V1.21

Sintetizador generativo DIY basado en RP2040. Genera audio evaluando fórmulas **bytebeat** representadas como árboles AST aleatorios, sample a sample, a 44100 Hz.

**Estado de release:** firmware `V1.21 / build v40` consolidado, con documentación y versionado alineados para publicación.

---

## Resumen

| Feature | Estado |
|---|---|
| Dual-core: Core0 audio @ 44100 Hz, Core1 control | ✓ |
| 15 pads capacitivos (matriz 3×5) | ✓ |
| 7 potenciómetros via CD4051 MUX (CH0–CH5 pots, CH6 CV IN, CH7 pot delay) | ✓ |
| 8 snapshots con persistencia en flash (con migración de versiones) | ✓ |
| DSP: Reverb · Chorus · HP Filter · Grain Freeze · Snap Gate · Beat Repeat · Delay tempo-sync | ✓ |
| MIDI IN/OUT (UART0, 31250 baud) | ✓ |
| Clock IN/OUT Eurorack (GP16/GP17) | ✓ |
| Note Mode — pads = grados de escala MIDI | ✓ |
| Encoder global: BPM / Swing / Root / Scale / Mutate | ✓ |
| Swing real en el clock interno | ✓ |
| Groove engine (MPC / Shuffle / Broken / Triplet / IDM + submodos Tight/Broken/Skitter/Melt) | ✓ |
| Spread estéreo optimizado por segmentos | ✓ |
| Feedback visual progresivo en WS2812 (8 LEDs, GP22) | ✓ |
| HOME reset: hold encoder 700 ms (SOFT) / 1500 ms (FULL) | ✓ |

---

## Hardware

### MCU
- **Raspberry Pi Pico** o **Ultimate Pico (16 MB flash)** — RP2040

### Audio
- **PCM5102A** (módulo I2S) — backend por defecto  
  `GP10 → BCK` · `GP11 → LCK/WS` · `GP12 → DIN`
- Backend PWM legacy disponible en `audio_output_pwm.h` (GP10/GP11)

### Pots y MUX
- **CD4051BE** DIP-16  
  `GP2 = S0` · `GP3 = S1` · `GP4 = S2` · `GP26 (ADC0) = COM`
- 7 × RV09 5 kΩ lineal: CH0–CH4 = P0–P4, CH5 = P5, CH7 = P6 (delay)
- **CH6 = CV IN** (protección: divisor 18 kΩ/33 kΩ + 2× BAT43 + 100 nF)

### Pads capacitivos
Matriz 3×5 — 15 pads activos

| | GP8 | GP9 | GP13 | GP14 | GP15 |
|---|---|---|---|---|---|
| **GP5** (ROW0) | REC | PLAY | SHIFT | SNAP1 | SNAP2 |
| **GP6** (ROW1) | MUTE | HAT | SNAP3 | SNAP4 | SNAP5 |
| **GP7** (ROW2) | KICK | SNARE | SNAP6 | SNAP7 | SNAP8 |

Cada ROW: `1 MΩ` serie al pad. `100 nF` cerámica entre ROW y GND por fila.  
COL pins: sin pull interno (`gpio_disable_pulls`) — crítico.

### Encoder
`GP19 = ENC_A` · `GP20 = ENC_B` · `GP21 = ENC_SW`

### LEDs
**WS2812B** barra 8 LEDs — `GP22` (PIO1, SM0)

### MIDI
`GP0 = TX (UART0)` · `GP1 = RX (UART0)` · 31250 baud  
MIDI IN: optoacoplador 6N138 (pin 8 → 3V3 obligatorio).  
MIDI OUT: transistor NPN (BC547).

### Clock Eurorack
`GP16 = CLOCK IN` (BAT43 clamp) · `GP17 = CLOCK OUT` (470 Ω serie)

---

## Mapeo de Controles

### Pots — 3 capas × 7 pots

| POT | Normal | SHIFT | SHIFT + REC |
|-----|--------|-------|-------------|
| P0 | Macro | Spread | Reverb Room |
| P1 | Tonal | Time Div | Reverb Wet |
| P2 | Drive | Beat Repeat Div | Chorus |
| P3 | Env Attack | Grain | Drum Decay |
| P4 | Env Release | Snap Gate | Drum Color |
| P5 | Glide | HP Filter | Duck Amount |
| P6 | **Delay Div** | Delay FB | Delay Wet |

> **P6 Delay**: la columna entera controla el delay. Delay div + wet se guardan en cada snapshot. Delay FB es bus global performático.

### Encoder

| Página | Giro | SHIFT + Giro | Click |
|--------|------|--------------|-------|
| BPM | ±1 BPM | ±10 BPM | — |
| SWING | swing amount | groove template (incluye IDM) | SHIFT+REC + encoder en IDM = submodo Tight/Broken/Skitter/Melt |
| ROOT | semitono | — | — |
| SCALE | escala | — | — |
| MUTATE | probabilidad del step | — | reset chance del step |

**Hold encoder:**
- **700 ms** → HOME SOFT: encoder→BPM, bus params → snapshot activo, pots → CATCHING
- **1500 ms** → HOME FULL: + mutes off, drum params → snapshot, beat repeat div reset

### Pads — Funciones

| Pad | Solo | SHIFT + pad | Descripción |
|-----|------|-------------|-------------|
| REC | Armar overdub | SHIFT+REC held = capa 2 de pots | — |
| PLAY | Arm/stop seq | SHIFT+PLAY = toggle Note Mode | — |
| SHIFT | Modificador | — | — |
| MUTE | Grain freeze wet (aftertouch) | SHIFT+MUTE = toggle Env Loop | — |
| KICK/SNARE/HAT | Trigger drum | MUTE+drum = mute por voz | SHIFT+KICK+SNARE = randomize |
| SNAP1–8 | Trigger snapshot | SHIFT+SNAP = punch FX momentáneo | — |
| SNAP8 (H) | Snapshot 8 | SHIFT+SNAP8 = snapshot arp | — |

### Punch FX (SHIFT + SNAP)

| SNAP | FX |
|------|-----|
| SNAP1 | Beat Repeat (div por P2 SHIFT) |
| SNAP2 | Beat Repeat 1/16 |
| SNAP3 | Beat Repeat 1/8 |
| SNAP4 | Beat Repeat 1/4 |
| SNAP5 | Grain Freeze |
| SNAP6 | Octava abajo |
| SNAP7 | Octava arriba |
| SNAP8 | Vibrato |

### Combos Globales

| Combo | Acción |
|-------|--------|
| SHIFT + REC + PLAY | Flash Save |
| SHIFT + KICK + SNARE | Randomize / Mutate global |
| MUTE + KICK/SNARE/HAT | Mute por drum |
| SHIFT + MUTE | Toggle Env Loop |
| SHIFT + PLAY | Toggle Note Mode |
| SHIFT + encoder (en MUTATE) | Ajustar chance del step actual |
| SHIFT + encoder click (en MUTATE) | Reset chance del step a 100% |

### Feedback LED (WS2812 barra 8 LEDs)

| Color | Significado |
|-------|-------------|
| Verde | Encoder → BPM |
| Amarillo | Encoder → Swing / Groove |
| Azul | Encoder → Root |
| Violeta | Encoder → Scale |
| Rojo | Encoder → Mutate |
| Cian (overlay) | Beat Repeat Div activo (800 ms) |
| Latido 1 Hz | Modo sin hardware de pads (`NO_PAD_HARDWARE`) |

---

## DSP Chain

```
Input bytebeat
    └→ DC Blocker
    └→ HP Filter (biquad)
    └→ Snap Gate (VCA periódico sincronizado al BPM)
    └→ Soft Clip / Drive
    └→ Chorus (BBD estéreo)
    └→ Grain Freeze
    └→ Reverb (Freeverb)
    └→ Delay (tempo-sync, 11 divisiones)
    └→ Limiter
    └→ PCM5102A I2S out
```

### Delay — Divisiones (@ 120 BPM, negra = 500 ms)

| Idx | División | Nombre | ms |
|-----|----------|--------|----|
| 0 | 1/8T | Corchea tresillo | 166 |
| 1 | 1/8 | Corchea | 250 |
| 2 | 1/8D | Corchea dotted | 375 |
| 3 | 1/4T | Negra tresillo | 333 |
| 4 | 1/4 | Negra (default) | 500 |
| 5 | 1/4D | Negra dotted | 750 |
| 6 | 1/2T | Blanca tresillo | 667 |
| 7 | 1/2 | Blanca | 1000 |
| 8 | 1/2D | Blanca dotted | 1500 |
| 9 | 3/4 | Tres tiempos | 1500 |
| 10 | 1/1 | Redonda | 2000 |

Buffer del delay: 32768 samples (~742 ms máximo). Divisiones > 742 ms se clampean al buffer.

### RAM (estimado, BSS estáticos)

| Componente | RAM |
|---|---|
| Reverb (Freeverb) | ~40 KB |
| Grain Freeze (estéreo) | 32 KB |
| Delay Line (mono int16) | 64 KB |
| Beat Repeat (mono int16) | 66 KB |
| **Total estimado** | **~202 KB** |
| **Disponible (RP2040)** | **264 KB** |

---

## Flash — Snapshot Persistence

- **Sector**: último sector Flash (offset `0x1FF000`, 4 KB)
- **Formato V4** (V1.20/V1.20.1): 8 × `PackedSnapshot` de 72 bytes = 576 bytes totales
- **Migración automática**: V2 → V3 → V4 al cargar firmware nuevo (datos anteriores se conservan)
- **CRC32** sobre el bloque completo — datos corruptos → defaults seguros

---

## Compilar

### GitHub Actions (recomendado)

Cada push a `main` ejecuta `.github/workflows/build.yml` y genera el UF2 automáticamente.
Descargar desde **Actions → Artifacts → bytebeat-machine-v1.20.1-uf2**.

## Publicar release

Crear y pushear un tag para adjuntar el `.uf2` a una release:

```bash
git tag v1.20.1
git push origin v1.20.1
```

### Local

```bash
# 1. Clonar Pico SDK
git clone --depth 1 --branch 2.1.0 https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk && git submodule update --init && cd ..

# 2. Exportar path
export PICO_SDK_PATH=$(pwd)/pico-sdk

# 3. Compilar
mkdir build && cd build
cmake .. -DPICO_BOARD=pico
make -j$(nproc)

# 4. Flashear — modo BOOTSEL
cp bytebeat_machine.uf2 /media/$(whoami)/RPI-RP2/
```

**Ultimate Pico (16 MB)**: si no aparece como `RPI-RP2`, mantener BOOTSEL más tiempo o usar:
```bash
picotool load build/bytebeat_machine.uf2
```

---

## Flashear

1. Mantener **BOOTSEL** presionado mientras conectás USB
2. Aparece como unidad `RPI-RP2`
3. Copiar `bytebeat_machine.uf2` a esa unidad
4. La placa reinicia automáticamente

---

## Changelog

- **V1.20.1**: release consolidada; documentación, artefactos y versionado alineados; fixes menores de estabilidad ya integrados en el firmware
- **V1.20**: nuevas páginas DENSITY / CHAOS / SPACE; edición de fills y conditions; direcciones FWD / REV / PEND / RAND; overlays LED semánticos
- **V1.19**: delay div+wet persisten en snapshot (flash V4); home_reset completo; bug fix migración flash
- **V1.18**: Beat Repeat sincronizado al BPM; pot map reorganizado (7 pots); stutter → beat repeat
- **V1.17**: 7mo pot (delay dedicado CH7); delay tempo-sync; fix arrays input_router [7]
- **V1.14**: encoder global, groove engine, swing real, spread estéreo optimizado
- **V1.10**: envelope AR con loop, flash store V3, sexta capa de control
- **V1.7**: Note Mode, control performático de drums, chorus, HP filter, grain freeze, snap gate
- **V1.0**: base dual-core, pads capacitivos, MIDI, clock I/O


## UX / Groove V2
- Nuevas páginas de encoder: DENSITY / CHAOS / SPACE
- SHIFT + REC + encoder en BPM: cambia dirección del secuenciador (FWD/REV/PEND/RAND)
- Density/Chaos ahora empujan el motor generativo y el comportamiento IDM
- SPACE controla reverb/delay/chorus de forma macro


## Submodos IDM

Cuando el groove está en `IDM`, podés usar `SHIFT + REC + encoder` en la página `SWING` para cambiar entre:

- `TIGHT`: más firme, ghost notes discretas
- `BROKEN`: más cortes y rebotes
- `SKITTER`: hats con micro retrigs rápidos
- `MELT`: feel más blando, lofi y stutter espacial

Click del encoder en ese contexto vuelve a `TIGHT`.


## Secuenciador y performance en V1.20
- edición de secuencia: **SHIFT + click encoder en SPACE** → toggle de **fill** del step actual
- edición de secuencia: **SHIFT + REC + encoder en DENSITY** → condición del step actual
  - `ALWAYS`
  - `EVERY_2`
  - `EVERY_4`
  - `RANDOM_50`
- los fills empujan ratchets/energía automáticamente, especialmente en groove **IDM**
- feedback visual de acciones ahora usa categorías semánticas (`copy`, `paste`, `clear`, `save arm`, `fill`, `condition`)


## Bytebeat-first pot remap (WIP)
- Base layer now prioritizes bytebeat behaviour.
- P0 = zone/formula family (live)
- P1 = rate (time div)
- P2 = tonal/shift proxy
- P3 = macro/mask proxy
- P4 = spread/feedback proxy
- P5 = tone (temporary HP until LP-clean-HP filter is added)
- P6 = seed variation (live)


## v37 note
- Added family-aware morph modes: linear, energetic crossfade, and safe morph for incompatible formula families.


## v38 note voice layer
- Note Mode no longer retunes the snapshot base engine.
- Added a parallel Bytebeat Note Voice Layer above the current snapshot.
- NOTE ON prepares a dedicated graph derived from Formula A and applies its own decay envelope.


## v39 note
- Note Voice Layer now uses env-macro-shaped decay/release.
- OLED shows active scale, root, and held note degree in NOTE MODE.


## Build v42 notes
- Added debounced hardware handling for the output mode switch.
- OLED init sequence hardened for Pico hardware bring-up.
- Output mode changes now publish a short OLED status message.



## Implementation note

The authoritative current pot/shift mapping lives in `io/input_router.cpp`.
Some older comments elsewhere in the tree may still reflect earlier control layouts.

## Implementation note

The authoritative live pot/shift mapping is defined in `io/input_router.cpp`.
Some older comments elsewhere in the tree may still describe earlier control layouts.
