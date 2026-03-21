OBJETIVO
- Validar SOLO que el PCM5102A entregue audio.

CABLEADO
- GP10 -> BCK
- GP11 -> LCK/LRCK
- GP12 -> DIN
- GND  -> GND
- VIN  -> probar primero con 5V si tu módulo es de los típicos con reguladores; si no, 3V3 también puede funcionar según versión.
- XSMT -> 3V3 fijo DESDE EL ARRANQUE
- SCK  -> GND
- FMT  -> LOW / I2S (no left-justified)
- FLT y DEMP: dejar en LOW / default

PRUEBA
- Debe sonar una cuadrada fija de 440 Hz en ambos canales.
- Si no suena: apagar COMPLETAMENTE, revisar soldaduras/jumpers del módulo, y volver a energizar.
- Probar con auriculares amplificados / parlantes activos / mixer de línea. El PCM5102A NO está pensado para parlante pasivo directo.
