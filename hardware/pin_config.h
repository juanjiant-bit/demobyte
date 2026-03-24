#pragma once
// Shared hardware pin definitions
// Encoder RGB LED support was removed; page feedback is shown on the WS2812 bar.
// OLED SSD1306 now uses hardware I2C on GPIO27/GPIO28, which are exposed on the
// standard Raspberry Pi Pico and avoid the invalid GP23/GP24 assignment.

#define PIN_NEOPIXEL    22
#define PIN_ONBOARD_LED 25   // GP25 = LED onboard del Pico (no Pico W)

#define OLED_SDA_PIN 27
#define OLED_SCL_PIN 28

// Legacy PWM backend is deprecated in this build; PCM5102 I2S is the primary path.
#define PIN_AUDIO_PWM_DEPRECATED 255

#define ENC_A_PIN  19
#define ENC_B_PIN  20
#define ENC_SW_PIN 21

// Output routing switch (LOW = split synth/drums, HIGH = full master)
#define OUTPUT_MODE_PIN 18
