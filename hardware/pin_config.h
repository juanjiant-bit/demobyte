#pragma once

// Hardware real ya construido:
// Ultimate Pi Pico + PCM5102A + 1 pote ADC + 4 pads capacitivos cobre.

#define PIN_ONBOARD_LED 25

// PCM5102A
#define PIN_I2S_BCLK 10
#define PIN_I2S_LRCK 11
#define PIN_I2S_DIN  12

// Potenciómetro directo a ADC0 (GP26)
#define PIN_POT_ADC 26
#define PIN_POT_ADC_INPUT 0

// Drive-sense capacitivo, fila común + columnas
#define PIN_PAD_ROW   5
#define PIN_PAD_SNAP  8
#define PIN_PAD_KICK  9
#define PIN_PAD_SNARE 13
#define PIN_PAD_HAT   14
