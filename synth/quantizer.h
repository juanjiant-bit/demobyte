#pragma once
// quantizer.h — Bytebeat Machine V1.21+
// Cuantizador y utilidades de escalas para lead y NOTE MODE.
//
// Mantiene API previa y agrega:
// - banco ampliado de escalas
// - nombres legibles de escala
// - mapeo de grados a ratio para NOTE MODE
//
// Todo pensado para ser CPU-liviano.

#include <cstdint>
#include <cmath>

enum class ScaleId : uint8_t {
    CHROMATIC   = 0,
    MAJOR       = 1,
    NAT_MINOR   = 2,
    DORIAN      = 3,
    PHRYGIAN    = 4,
    LYDIAN      = 5,
    MIXOLYDIAN  = 6,
    LOCRIAN     = 7,
    HARM_MINOR  = 8,
    MELOD_MINOR = 9,
    PENTA_MAJ   = 10,
    PENTA_MIN   = 11,
    BLUES       = 12,
    WHOLETONE   = 13,
    DIM_HALF    = 14,   // diminished half-whole
    DIM_WHOLE   = 15,   // diminished whole-half
    HIJAZ       = 16,
    IN_SEN      = 17,
    PELOG       = 18,
    NUM_SCALES  = 19
};

struct ScaleDef {
    const char* name;
    uint8_t length;
    uint8_t degrees[8];   // hasta 8 grados; los no usados se ignoran
    uint16_t mask12;      // máscara de 12 semitonos, útil para quantize()
};

struct Quantizer {
    static constexpr uint8_t SCALE_COUNT = (uint8_t)ScaleId::NUM_SCALES;

    // Banco ampliado de escalas
    static constexpr ScaleDef SCALE_DEFS[SCALE_COUNT] = {
        {"CHROM", 8, {0,1,2,3,4,5,6,7},            0b111111111111},
        {"MAJOR", 8, {0,2,4,5,7,9,11,12},          0b101010110101},
        {"MINOR", 8, {0,2,3,5,7,8,10,12},          0b010110101101},
        {"DORIA", 8, {0,2,3,5,7,9,10,12},          0b011010101101},
        {"PHRYG", 8, {0,1,3,5,7,8,10,12},          0b010110101011},
        {"LYDIA", 8, {0,2,4,6,7,9,11,12},          0b101011010101},
        {"MIXOL", 8, {0,2,4,5,7,9,10,12},          0b011010110101},
        {"LOCRI", 8, {0,1,3,5,6,8,10,12},          0b010101101011},
        {"HARMM", 8, {0,2,3,5,7,8,11,12},          0b100110101101},
        {"MELMM", 8, {0,2,3,5,7,9,11,12},          0b101010101101},
        {"PMAJ",  6, {0,2,4,7,9,12,0,0},           0b001010010101},
        {"PMIN",  6, {0,3,5,7,10,12,0,0},          0b010010101001},
        {"BLUES", 7, {0,3,5,6,7,10,12,0},          0b010011101001},
        {"WHOLE", 7, {0,2,4,6,8,10,12,0},          0b010101010101},
        {"DIMHW", 8, {0,1,3,4,6,7,9,10},           0b011010110011},
        {"DIMWH", 8, {0,2,3,5,6,8,9,11},           0b101101101101},
        {"HIJAZ", 8, {0,1,4,5,7,8,10,12},          0b010110110011},
        {"INSEN", 6, {0,1,5,7,10,12,0,0},          0b010010100011},
        {"PELOG", 6, {0,1,3,7,8,12,0,0},           0b000110100011},
    };

    // Compatibilidad: tabla de máscaras derivada de SCALE_DEFS
    static constexpr uint16_t SCALE_MASKS[SCALE_COUNT] = {
        0b111111111111,
        0b101010110101,
        0b010110101101,
        0b011010101101,
        0b010110101011,
        0b101011010101,
        0b011010110101,
        0b010101101011,
        0b100110101101,
        0b101010101101,
        0b001010010101,
        0b010010101001,
        0b010011101001,
        0b010101010101,
        0b011010110011,
        0b101101101101,
        0b010110110011,
        0b010010100011,
        0b000110100011,
    };

    static float note_to_freq(uint8_t note) {
        if (note > 127) note = 127;
        return 440.0f * std::pow(2.0f, (float(int(note) - 69)) / 12.0f);
    }

    static uint8_t quantize(uint8_t pitch_raw, uint8_t scale_id, uint8_t root) {
        if (scale_id >= SCALE_COUNT) scale_id = (uint8_t)ScaleId::MAJOR;
        root %= 12u;
        const uint16_t mask = SCALE_MASKS[scale_id];
        uint8_t octave = pitch_raw / 12u;
        uint8_t semitone = (pitch_raw + 12u - root) % 12u;

        if (mask & (1u << semitone)) {
            uint8_t quantized = (uint8_t)(octave * 12u + root + semitone);
            if (quantized > 127u) quantized = 127u;
            return quantized;
        }

        uint8_t best = semitone;
        uint8_t best_dist = 12u;
        for (int8_t d = 1; d <= 6; ++d) {
            uint8_t dn = (uint8_t)((semitone + 12 - d) % 12);
            if (mask & (1u << dn)) { best = dn; best_dist = (uint8_t)d; break; }
        }
        for (int8_t d = 1; d <= 6; ++d) {
            uint8_t up = (uint8_t)((semitone + d) % 12);
            if (mask & (1u << up)) {
                if ((uint8_t)d < best_dist) { best = up; best_dist = (uint8_t)d; }
                break;
            }
        }

        uint8_t quantized = (uint8_t)(octave * 12u + root + best);
        if (quantized > 127u) quantized = (quantized >= 12u) ? uint8_t(quantized - 12u) : 127u;
        if (quantized < 24u) quantized = uint8_t(quantized + 12u);
        return quantized;
    }

    static float blend_freq(uint8_t pitch_raw, uint8_t pitch_q, float amount) {
        float f_raw = note_to_freq(pitch_raw);
        float f_q   = note_to_freq(pitch_q);
        return f_raw + (f_q - f_raw) * amount;
    }

    static float pot_to_amount(float pot) {
        if (pot < 0.25f) return 0.0f;
        if (pot < 0.60f) return (pot - 0.25f) / 0.35f * 0.6f;
        return 0.6f + (pot - 0.60f) / 0.40f * 0.4f;
    }

    static constexpr uint8_t FAVORITE_ROOTS[5] = { 0, 3, 5, 7, 10 };

    static const char* scale_name(ScaleId s) {
        const uint8_t idx = (uint8_t)s;
        return (idx < SCALE_COUNT) ? SCALE_DEFS[idx].name : "?";
    }
    static const char* scale_name(uint8_t scale_id);

    static const char* root_name(uint8_t root) {
        static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        return names[root % 12u];
    }

    static float semitone_to_ratio(uint8_t semitone) {
        return std::pow(2.0f, float(semitone) / 12.0f);
    }

    static float note_mode_degree_to_ratio(uint8_t scale_id, uint8_t root, uint8_t degree);
};
