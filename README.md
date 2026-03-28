
# Minimal Drum Validation

Base mínima para validar:
- I2S PCM5102
- pads con trigger/gate/aftertouch
- 4 drums simples y separados
- serial debug de pads

## Pinout

### I2S PCM5102
- GP10 -> BCLK
- GP11 -> LRCK
- GP12 -> DIN

### Pots
- GP26 -> Volume
- GP27 -> Decay global drums
- GP28 -> Tone / brightness drums

### Pads
- GP8  -> Pad 1 -> Kick
- GP9  -> Pad 2 -> Snare
- GP13 -> Pad 3 -> Hat
- GP14 -> Pad 4 -> Perc

## Comportamiento
- Pad 1: kick
- Pad 2: snare
- Pad 3: hihat
- Pad 4: perc/tom

Serial cada 100 ms:
- raw
- pressure
- trigger
- pressed
