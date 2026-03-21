TEST DAC PCM5102A - ESCALERA DE FRECUENCIAS

Objetivo:
Validar de forma auditiva e inequívoca la salida de audio del PCM5102A.

Pinout usado:
- GP10 -> BCK
- GP11 -> LCK / LRCK
- GP12 -> DIN
- XSMT -> 3.3V
- SCK -> GND
- GND comun entre Pico y DAC
- VIN del DAC segun el modulo (3.3V o 5V), pero solo si el modulo enciende correctamente

Comportamiento esperado:
- LED onboard del Pico parpadea
- En la salida de audio se escucha una escalera ascendente repetitiva:
  220 Hz -> 330 Hz -> 440 Hz -> 550 Hz -> 660 Hz -> 880 Hz -> 1100 Hz -> 1320 Hz
- Cada escalon dura aprox 350 ms y luego la secuencia vuelve a empezar

Interpretacion:
- Si se escucha claramente la secuencia ascendente repetida, el DAC y el stream I2S estan funcionando
- Si solo hay zumbido, clock bleed o ruido fijo, el DAC no esta recibiendo audio valido
- Si no hay nada de audio pero el DAC prende, revisar clocks, formato I2S y salida analogica
