# RP2040 + PCM5102 minimal I2S hardware test

Proyecto mínimo para validar un DAC PCM5102 con un RP2040 usando PIO y **sin DMA**.

## Pines

- GP10 -> BCK
- GP11 -> LRCK / LCK
- GP12 -> DIN
- GND común
- XSMT -> 3.3V **obligatorio**

## Qué hace

Genera una onda cuadrada continua en ambos canales para comprobar rápidamente si el enlace I2S funciona.

## Formato usado

Este test usa **Philips I2S real con slots de 32 bits por canal**.

Eso significa:
- `BCLK = Fs * 64`
- 16 bits de audio útiles
- cada sample de 16 bits se envía **alineado a la izquierda** dentro de un slot de 32 bits
- se escribe **un word de 32 bits por canal**

### Por qué no empaqueto L+R en un único `uint32_t`

Porque para respetar correctamente el desfase de 1 bit de Philips I2S con una implementación PIO mínima y clara, lo más robusto es emitir **un slot estándar de 32 bits por canal**.

Para el PCM5102 esto suele ser más seguro que intentar una variante compacta 16+16 dentro de un único word.

## Compilación

Asumiendo que ya tenés instalado el Pico SDK:

```bash
mkdir build
cd build
cmake ..
make -j4
```

Esto genera `.uf2`, `.elf`, `.bin` y el header del PIO automáticamente.

## Notas

- No usa MCLK/SCK. El PCM5102 puede reconstruir clock internamente desde BCK/LRCK.
- Si no escuchás nada, revisá primero:
  - XSMT a 3.3V
  - masa común
  - DIN/BCK/LRCK en el orden correcto
  - que el módulo no esté muteado por jumper o soldadura
