# Dem0!Byt3 V12 – baseline mejorado sobre V11

Esta versión conserva el hardware validado de V11:
- PCM5102 por I2S
- pads resistivos en GP8/9/13/14
- pots en GP26/27/28

Cambios principales:
- MORPH ahora mezcla voz A ↔ voz B reales
- TIMBRE controla la complejidad interna de ambas voces
- smoothing corto en pots para evitar zipper sin meter lag grande
- randomización en familias más musicales
- sidechain más moderado
- master con LP global + HP suave + softclip final

Pad mapping:
- Pad 1: randomiza la voz menos dominante
- Pad 2: kick
- Pad 3: snare
- Pad 4: hihat
