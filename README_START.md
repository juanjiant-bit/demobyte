# Dem0Byt3 V13 Clean Start

Esta base arranca desde el hardware ya validado:

- PCM5102 I2S validado previamente
- BCLK = GP10
- DIN  = GP12
- Pot  = GP26
- Pads capacitivos sobre tu hardware de prueba

## Idea
Separar el sistema en 4 capas:

1. `io/pads.*`  
   Lee pads y entrega `trigger / hold / pressure`

2. `synth/bytebeat_engine.*`  
   Render principal del bytebeat, limpio y controlable

3. `drums/drum_engine.*`  
   Kick, snare y hat separados

4. `master/master.*`  
   DC block, compresión simple, make-up gain y softclip

## Importante
Para no romper la configuración I2S que ya validaste, esta base **no reemplaza**
tu implementación conocida del DAC. Copiá desde tu baseline estos archivos ya probados:

- `audio/audio_output_i2s.h`
- `audio/audio_output_i2s.cpp`
- `audio/pcm5102_i2s.pio`

y dejalos en sus rutas.

Después compilás este proyecto usando esa salida conocida.

## Mapeo actual
- Pad 1: toggle drone + randomiza fórmula en trigger
- Pad 2: kick
- Pad 3: snare
- Pad 4: hat
- Pot: macro morph / tone / drive
