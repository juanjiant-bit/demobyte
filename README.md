# yuy0 simple validation v3 hybrid

Versión mínima para validar el hardware ya construido:

- Ultimate Pi Pico / RP2040
- PCM5102A por I2S
- 1 pot por ADC
- 4 pads capacitivos de cobre, esquema drive-sense
- LED onboard

## Pinout

- GP10 -> BCK PCM5102A
- GP11 -> LCK/LRCK PCM5102A
- GP12 -> DIN PCM5102A
- GP26 -> pote ADC0
- GP5  -> fila común de pads + 100nF a GND
- GP8  -> pad SNAP
- GP9  -> pad KICK
- GP13 -> pad SNARE
- GP14 -> pad HAT
- GP25 -> LED onboard

## Qué hace

- audio simple por I2S al DAC
- el pote cambia timbre y respuesta del motor
- los 4 pads disparan percusión y además abren voz tonal
- impresión por USB serial de `adc`, `pads`, `delta/thr` y `raw/base`

## Qué tomé de cada rama

De la rama simple previa:
- motor chico y fácil de tocar
- single-core
- sin secuencer, MIDI, OLED ni multicore

Del zip `byt3_validate`:
- enfoque de calibración y lectura más robusta para pads capacitivos
- descarga explícita del pin de columna antes de medir
- baseline adaptativo + threshold dependiente del ruido

## Uso

1. Energizar la placa sin tocar pads durante el arranque.
2. Confirmar por serial que aparecen lecturas estables.
3. Probar un pad por vez.
4. Si algún pad está muy sensible o no dispara, ajustar en `io/cap_pad_hybrid.h` / `main.cpp`.
