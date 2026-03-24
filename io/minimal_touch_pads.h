#pragma once

#include <cstdint>

// Minimal 1x4 capacitive pad reader for the already-built prototype.
// Wiring confirmed from the latest sketch/photo:
//   DRIVE/ROW0 -> GP5
//   PAD1       -> GP8   (bytebeat drone/random / SNAP)
//   PAD2       -> GP9   (kick)
//   PAD3       -> GP13  (snare)
//   PAD4       -> GP14  (hat)
// Same measurement idea as the matrix handler: charge time from one drive pin
// into each sense pad independently.
class MinimalTouchPads {
public:
    static constexpr uint8_t NUM_PADS = 4;
    static constexpr uint8_t DRIVE_PIN = 5;
    static constexpr uint8_t PAD_PINS[NUM_PADS] = {8, 9, 13, 14};

    struct Preset {
        uint32_t discharge_us;
        uint32_t max_charge_us;
        uint32_t hyst_on_us;
        uint32_t hyst_off_us;
        uint8_t calib_samples;
        float baseline_alpha;

        // PROTOTYPE_SMALL: pistas PCB pequeñas (~5x5mm) — discharge corto
        static constexpr Preset PROTOTYPE_SMALL() {
            return {220, 2600, 20, 10, 220, 0.0018f};
        }

        // PROTOTYPE_LARGE: pads interdigitados grandes de cobre grabado
        // Con 1MΩ en COL, pads de ~30x50mm con patrón interdigitado:
        //
        // RC estimado: 1MΩ × 50nF = 50ms
        // discharge=10ms: deja 82% de carga (OK con thresholds relativos)
        // max_charge=150ms: cubre pads muy grandes o con mucha capacitancia
        //
        // hyst_on/off = 0 → scan() usa 15%/8% del baseline individual
        // Esto compensa la variación de baseline entre pads de distinto tamaño.
        //
        // Tiempos con este preset:
        //   Calibración (20 muestras): ~7s (una sola vez al arrancar)
        //   Scan loop: ~200ms para 4 pads = ~5Hz de respuesta
        //   Latencia: perceptible (~200ms) pero funcional para texturas
        //
        // Para hardware final (JLCPCB, pads 10x10mm):
        //   usar discharge=1000, max=20000 → scan <10ms = 100Hz
        static constexpr Preset PROTOTYPE_LARGE() {
            return {10000, 150000, 0, 0, 20, 0.008f};
            // hyst_on/off en 0: calculados como % del baseline en scan()
        }

        static constexpr Preset PROTOTYPE() {
            return PROTOTYPE_LARGE();  // default para hardware actual
        }
    };

    void init(Preset p = Preset::PROTOTYPE());
    void calibrate();
    void scan();

    bool is_pressed(uint8_t pad) const;
    bool just_pressed(uint8_t pad) const;
    bool just_released(uint8_t pad) const;
    uint16_t get_state() const { return state_confirmed_; }
    uint32_t get_raw_us(uint8_t pad) const;
    uint32_t get_baseline_us(uint8_t pad) const;

private:
    uint32_t measure_charge_us(uint8_t pin);

    Preset preset_{};
    float baseline_f_[NUM_PADS] = {};
    uint32_t raw_us_[NUM_PADS] = {};
    uint16_t state_confirmed_ = 0;
    uint16_t state_prev_ = 0;
};
