#pragma once
// pin_config.h — BYT3 VALIDATE
// Pinout para la placa construida (Ultimate Pi Pico + PCM5102A + 4 pads capacitivos)

// ── I2S DAC (PCM5102A) ────────────────────────────────────────
#define PIN_I2S_BCK   10   // BCK
#define PIN_I2S_LRCK  11   // LCK/WS  (debe ser BCK+1)
#define PIN_I2S_DIN   12   // DIN

// ── Pads capacitivos (drive-sense, cobre) ─────────────────────
// ROW único GP5, 1MΩ serie por pad, 100nF a GND en fila
#define PAD_ROW_PIN    5

// COL pins — sin pull interno (CRÍTICO)
#define PAD_COL_SNAP   8   // PAD1
#define PAD_COL_KICK   9   // PAD2
#define PAD_COL_SNARE 13   // PAD3
#define PAD_COL_HAT   14   // PAD4
#define NUM_PADS_VAL   4

// ── ADC pote (directo, sin MUX) ───────────────────────────────
#define POT_PIN       26   // GP26 = ADC0

// ── LED onboard ───────────────────────────────────────────────
#define PIN_LED       25
