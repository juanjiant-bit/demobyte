#pragma once
// note_mode.h — Bytebeat Machine V1.21
// Tabla de mapeo pad → grado de escala → nota MIDI cuantizada.
// Completamente stateless — solo funciones y tablas constantes.
//
// MAPEO (pads 0–7 con escala activa):
//   PAD0 → grado 1  (root)
//   PAD1 → grado 2
//   PAD2 → grado 3
//   PAD3 → grado 4
//   PAD4 → grado 5
//   PAD5 → grado 6
//   PAD6 → grado 7
//   PAD7 → root + 12 (octava)
//
// Octava base: 4 (C4 = nota MIDI 60 = la nota MIDI base para root=C).
// Las notas se cuantizan con Quantizer::quantize() — siempre en escala.
//
// pitch_ratio: freq_nota / freq_A4(440Hz)
// El AudioEngine usa ctx.t * pitch_ratio para modular el bytebeat.
//
#include <cstdint>
#include "quantizer.h"

struct NoteMode {
    // Octava base MIDI (C4 = 60)
    static constexpr uint8_t OCTAVE_BASE = 60;   // C4
    static constexpr uint8_t NUM_PADS    = 8;

    // Mapear pad 0–7 → nota MIDI cuantizada (scale+root)
    // pad7 = octava siempre (root + 12, no necesita cuantizar)
    static uint8_t pad_to_midi(uint8_t pad, ScaleId scale, uint8_t root) {
        if (pad >= NUM_PADS) return OCTAVE_BASE;
        const uint8_t sid = (uint8_t)scale;
        if (sid >= Quantizer::SCALE_COUNT) scale = ScaleId::MAJOR;
        const auto& def = Quantizer::SCALE_DEFS[(uint8_t)scale];

        if (pad == 7) {
            uint8_t base = uint8_t(OCTAVE_BASE + root);
            return (base + 12u <= 127u) ? uint8_t(base + 12u) : base;
        }

        if (pad < def.length) {
            uint8_t midi = uint8_t(OCTAVE_BASE + root + def.degrees[pad]);
            return (midi <= 127u) ? midi : 127u;
        }

        // Fallback para escalas con menos grados visibles: reciclar grados en la siguiente octava.
        const uint8_t recycled = uint8_t((pad - def.length) % def.length);
        uint8_t midi = uint8_t(OCTAVE_BASE + root + def.degrees[recycled] + 12u);
        return (midi <= 127u) ? midi : 127u;
    }

    // Nota MIDI → ratio de pitch relativo a A4 (440Hz)
    // ratio = 2^((midi - 69) / 12)
    // Implementado con tabla de 128 entradas precalculadas (float, 512 bytes).
    static float midi_to_pitch_ratio(uint8_t midi_note) {
        // Approx: usamos la tabla de Quantizer::note_to_freq / 440.0
        float freq = Quantizer::note_to_freq(midi_note);
        return freq / 440.0f;
    }

    // Nota MIDI → nombre (para debug/display)
    static const char* midi_note_name(uint8_t note) {
        static const char* NAMES[12] = {
            "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
        };
        return NAMES[note % 12];
    }
};
