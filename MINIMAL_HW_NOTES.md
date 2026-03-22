# BYT3 minimal hardware adaptation

Este branch de trabajo deja intacto el firmware principal y agrega una variante de validación para el prototipo mínimo ya construido.

## Mapa asumido desde tu esquema

- GP10 -> PCM5102 BCK
- GP11 -> PCM5102 LRCK/LCK
- GP12 -> PCM5102 DIN
- GP26 -> pot macro / morph
- GP2  -> línea común de excitación para pads capacitivos
- GP8  -> PAD 1 / drone random
- GP9  -> PAD 2 / kick
- GP14 -> PAD 3 / snare
- GP15 -> PAD 4 / hat

## Qué hace esta variante

- Conserva exactamente el backend I2S validado con PCM5102.
- Reemplaza la matriz 3x5 y el mux por una lectura capacitiva mínima 1x4.
- El pote controla a la vez:
  - macro global del engine
  - morph entre formula A y formula B
  - color/decay/duck de los drums
  - drive / filter / resonance del bytebeat
- PAD 1 randomiza los parámetros del bytebeat alrededor del estado actual y retriggera el engine.
- PAD 2 = kick
- PAD 3 = snare
- PAD 4 = hat
- El kick sigue entrando al sidechain del bytebeat usando `PARAM_DUCK_AMOUNT`.

## Build

```bash
mkdir build
cd build
cmake .. -DPICO_BOARD=pico -DBYT3_MINIMAL_HW=ON
make -j4
```

El UF2 resultante es `bytebeat_machine_minimal.uf2`.

## Ajustes probables en banco

Si la sensibilidad táctil del prototipo queda baja o muy nerviosa, tocar en `io/minimal_touch_pads.h`:

- `hyst_on_us`
- `hyst_off_us`
- `discharge_us`
- `baseline_alpha`

Si el pinout real del prototipo no coincide exactamente con el croquis, cambiar solo:

- `DRIVE_PIN`
- `PAD_PINS[]`

sin tocar el resto del engine.


## Latest pad wiring confirmation

Based on the updated sketch/photo:

- GP5 = common capacitive excitation line (`ROW0`)
- GP8 = Pad 1 / drone-random (`SNAP`)
- GP9 = Pad 2 / kick
- GP13 = Pad 3 / snare
- GP14 = Pad 4 / hat
- GP10/GP11/GP12 = PCM5102 I2S (`BCK/LCK/DIN`)
- GP26 = potentiometer

Touch preset was made a bit slower and more tolerant for the long copper traces and the shared-bus handmade pad board.
