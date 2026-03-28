# Dem0Byt3 Pad + I2S minimal test

Base mínima para iterar ordenadamente:
- salida I2S PCM5102 aislada
- detección de pads de `PADS PERFECTOS`
- 4 ticks simples para verificar trigger, gate y aftertouch
- sin audio engine bytebeat / floatbeat / drums / master

## Qué hace
- Pad 1: tick grave
- Pad 2: tick medio
- Pad 3: tick agudo
- Pad 4: tick de ruido
- El volumen usa el pote de GP26 solo para checkear que la lectura funciona
- GP27 y GP28 quedan leídos e impresos por serial como referencia, pero no alteran el audio

## Hardware / pinout
### RP2040 -> PCM5102A (I2S)
- GP10 = BCLK
- GP11 = LRCK
- GP12 = DIN
- GND común
- 3V3 o 5V según tu módulo/adaptación

### Pads resistivos / táctiles
- Pad 1 = GP8
- Pad 2 = GP9
- Pad 3 = GP13
- Pad 4 = GP14

### Potes
- Volumen = GP26 / ADC0
- Morph ref = GP27 / ADC1
- Color ref = GP28 / ADC2

## Notas de iteración
1. Primero validar I2S limpio
2. Después validar pads (trigger / gate / hold / aftertouch)
3. Recién después volver a sumar audio engine real

## Observación
Esta base está hecha para diagnóstico. No busca sonar lindo: busca que cada bloque estructural quede aislado y confiable.
