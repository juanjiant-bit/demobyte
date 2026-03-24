#pragma once
// io/control_frame.h — BYT3/YUY0 V1.22
//
// ControlFrame: snapshot inmutable del estado de hardware para un frame de control.
//
// Se construye UNA VEZ al inicio de cada iteración de Core1 desde
// CapPadHandler y AdcHandler, y se pasa a todos los handlers.
//
// Ventajas:
//   - Timestamp único por frame: no hay drift entre lecturas de now_ms
//   - InputRouter deja de llamar a to_ms_since_boot() internamente
//   - handle_snapshots no re-lee get_just_released() desde el handler
//   - Testeable sin hardware: construirlo a mano en tests
//   - Desacopla la frecuencia de scan (~200Hz) del uso lógico

#include <cstdint>
#include "cap_pad_handler.h"
#include "adc_handler.h"

struct ControlFrame {
    // ── Pads ─────────────────────────────────────────────────────
    uint16_t pads_state;          // bitmask: estado confirmado en este frame
    uint16_t pads_just_pressed;   // bits que pasaron de 0→1 este frame
    uint16_t pads_just_released;  // bits que pasaron de 1→0 este frame
    float    pad_pressure[CapPadHandler::NUM_PADS];  // aftertouch [0.0, 1.0]

    // ── ADC / Pots ───────────────────────────────────────────────
    float    pots[AdcHandler::NUM_POTS];  // valores suavizados [0.0, 1.0]
    float    cv;                           // CV IN suavizado [0.0, 1.0]
    bool     cv_active;                    // true si cv > noise floor

    // ── Timestamp único del frame ────────────────────────────────
    uint32_t now_ms;

    // ── Helpers inline ───────────────────────────────────────────
    bool is_pressed(uint8_t pad) const {
        return (pad < CapPadHandler::NUM_PADS) && ((pads_state >> pad) & 1u);
    }
    bool just_pressed(uint8_t pad) const {
        return (pad < CapPadHandler::NUM_PADS) && ((pads_just_pressed >> pad) & 1u);
    }
    bool just_released(uint8_t pad) const {
        return (pad < CapPadHandler::NUM_PADS) && ((pads_just_released >> pad) & 1u);
    }
    float pressure(uint8_t pad) const {
        return (pad < CapPadHandler::NUM_PADS) ? pad_pressure[pad] : 0.0f;
    }
    float pot(uint8_t idx) const {
        return (idx < AdcHandler::NUM_POTS) ? pots[idx] : 0.0f;
    }
};

// ── Builder ──────────────────────────────────────────────────────
// Llamar una vez al inicio del loop de Core1, con los timers de scan
// ya corriendo (pad_timer_cb y adc_timer_cb manejados por interrupciones).
#include "pico/stdlib.h"

inline ControlFrame build_control_frame(CapPadHandler& pads, AdcHandler& adc) {
    ControlFrame f;
    f.now_ms           = to_ms_since_boot(get_absolute_time());
    f.pads_state       = pads.get_state();
    f.pads_just_pressed  = pads.get_just_pressed();
    f.pads_just_released = pads.get_just_released();
    for (uint8_t i = 0; i < CapPadHandler::NUM_PADS; ++i)
        f.pad_pressure[i] = pads.get_pressure(i);
    for (uint8_t i = 0; i < AdcHandler::NUM_POTS; ++i)
        f.pots[i] = adc.get(i);
    f.cv        = adc.get_cv();
    f.cv_active = adc.cv_active();
    return f;
}
