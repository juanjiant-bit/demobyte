# demobyte_i2s_pot_pads

Firmware mínimo de validación para tu hardware actual:

- **I2S + PCM5102** funcionando en RP2040 por PIO
- **1 pot** leído por el **CD4051**
- **pads capacitivos 3x5** usando el mismo pinout del stage11
- build por **GitHub Actions** con artifact `.uf2`

## Pinout usado

### Audio I2S
- `GP10 -> BCK`
- `GP11 -> LRCK / LCK`
- `GP12 -> DIN`
- `XSMT -> 3.3V`
- `GND común`

### MUX de pots (CD4051)
- `GP2 = S0`
- `GP3 = S1`
- `GP4 = S2`
- `GP26 = ADC0 / COM`

En esta demo se usa **P0 = CH0** para controlar la frecuencia.

### Pads capacitivos (matriz 3x5)
Rows:
- `GP5 = ROW0`
- `GP6 = ROW1`
- `GP7 = ROW2`

Cols:
- `GP8  = COL0`
- `GP9  = COL1`
- `GP13 = COL2`
- `GP14 = COL3`
- `GP15 = COL4`

Matriz física:

|      | GP8 | GP9 | GP13 | GP14 | GP15 |
|------|-----|-----|------|------|------|
| GP5  | REC | PLAY | SHIFT | SNAP1 | SNAP2 |
| GP6  | MUTE | HAT | SNAP3 | SNAP4 | SNAP5 |
| GP7  | KICK | SNARE | SNAP6 | SNAP7 | SNAP8 |

Circuito esperado por pad:
- `ROW -- 1MΩ -- PAD -- COL`
- `100nF` entre cada **ROW** y **GND**
- columnas **sin pull interno**

## Qué hace esta demo

- Al arrancar, **core 0** se dedica solo al audio I2S.
- **core 1** escanea el MUX y los pads para no cortar el audio.
- El **pot CH0** controla la frecuencia del tono.
- Si tocás pads, la demo fuerza frecuencias conocidas para validar la matriz.
- `SHIFT` hace una prueba estéreo: izquierda = frecuencia del pote, derecha = 2x.

## Mapa de prueba de pads

- `REC`   -> 110 Hz
- `PLAY`  -> 220 Hz
- `SHIFT` -> estéreo split (L=pot, R=2x)
- `SNAP1` -> C4
- `SNAP2` -> D4
- `MUTE`  -> E4 (además baja un poco el volumen)
- `HAT`   -> F4
- `SNAP3` -> G4
- `SNAP4` -> A4
- `SNAP5` -> B4
- `KICK`  -> C5
- `SNARE` -> D5
- `SNAP6` -> E5
- `SNAP7` -> F5
- `SNAP8` -> G5

## Uso

1. Encendé el equipo **sin tocar los pads** durante el arranque.
2. Esperá la calibración inicial de pads.
3. Mové el pot en `CH0` y escuchá cómo cambia la frecuencia.
4. Tocá pads y comprobá que cada uno dispara un tono distinto.
5. Tocá `SHIFT` para confirmar que L y R se separan.

## GitHub Actions

El workflow incluido compila y sube un artifact:
- `demobyte-i2s-pot-pads-uf2`

## Nota importante

Esta demo está pensada para **validación de hardware**, no para performance final.
El escaneo capacitivo y del MUX va en el segundo core para que el audio I2S siga sonando limpio sin DMA.
